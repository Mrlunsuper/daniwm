/* Phase 0 semantic baseline: simple Xlib clients, private display only.
 * Synthetic root keys exercise WM actions; this is not physical-input proof. */
#define _POSIX_C_SOURCE 200809L
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static Display *display;
static Window root, time_window;
static const char *private_display, *comp_path;
static pid_t comp_pid;
static unsigned query_races;
static int xerror(Display *d, XErrorEvent *e) {
    /* Root scans race with the WM destroying its overview. Retry polls;
     * never suppress other protocol errors or use stale attributes. */
    if (e->error_code == BadWindow && e->request_code == 3) { query_races++; return 0; }
    char message[128];
    XGetErrorText(d, e->error_code, message, sizeof(message));
    fprintf(stderr, "FAIL: WM baseline X error %s req=%u/%u\n", message,
            e->request_code, e->minor_code);
    exit(1);
}
static void tick(void) {
    struct timespec t = {0, 10000000};
    nanosleep(&t, NULL);
}
static void cleanup(void) {
    if (comp_pid > 0) {
        kill(comp_pid, SIGTERM);
        waitpid(comp_pid, NULL, 0);
        comp_pid = 0;
    }
}
static void fail(const char *message) {
    fprintf(stderr, "FAIL: animation WM baseline: %s\n", message);
    exit(1);
}
static unsigned long property(Window w, const char *name, Atom expected) {
    Atom type;
    int format;
    unsigned long n, rest, result = ~0UL;
    unsigned char *data = NULL;
    if (XGetWindowProperty(display, w, XInternAtom(display, name, False),
        0, 1, False, expected, &type, &format, &n, &rest, &data) == Success &&
        type == expected && format == 32 && n == 1) result = *(unsigned long *)data;
    if (data) XFree(data);
    return result;
}
static void await_property(Window w, const char *name, Atom type, unsigned long value) {
    for (unsigned i = 0; i < 300; i++) {
        if (property(w, name, type) == value) return;
        tick();
    }
    fail(name);
}
static int mapped(Window w) {
    XWindowAttributes a;
    return XGetWindowAttributes(display, w, &a) && a.map_state == IsViewable;
}
static void await_mapping(Window w, int expected) {
    for (unsigned i = 0; i < 300; i++) {
        if (mapped(w) == expected) return;
        tick();
    }
    fail("mapping did not settle");
}
static void key(Window window, KeySym sym, unsigned modifiers) {
    XEvent e = {0};
    e.xkey.type = KeyPress; e.xkey.display = display;
    e.xkey.window = window; e.xkey.root = root;
    e.xkey.same_screen = True; e.xkey.state = modifiers;
    /* Obtain a fresh server timestamp; synthetic CurrentTime key events do
     * not model real input ordering during focus/unmap transitions. */
    Atom stamp = XInternAtom(display, "_DANIWM_TEST_TIME", False);
    unsigned char byte = 1;
    XChangeProperty(display, time_window, stamp, XA_INTEGER, 8, PropModeReplace, &byte, 1);
    XEvent property_event;
    XWindowEvent(display, time_window, PropertyChangeMask, &property_event);
    e.xkey.time = property_event.xproperty.time;
    e.xkey.keycode = XKeysymToKeycode(display, sym);
    assert(e.xkey.keycode);
    assert(XSendEvent(display, window, False, KeyPressMask, &e));
    XFlush(display);
}
static Window overview(void) {
    Window rr, parent, *children = NULL, result = None;
    unsigned count;
    assert(XQueryTree(display, root, &rr, &parent, &children, &count));
    for (unsigned i = 0; i < count; i++) {
        XWindowAttributes a;
        if (XGetWindowAttributes(display, children[i], &a) && a.override_redirect &&
            a.class == InputOutput && a.map_state == IsViewable &&
            a.width == DisplayWidth(display, DefaultScreen(display)) &&
            a.height == DisplayHeight(display, DefaultScreen(display))) result = children[i];
    }
    if (children) XFree(children);
    return result;
}
static Window await_overview(int expected) {
    for (unsigned i = 0; i < 300; i++) {
        Window w = overview();
        if (!!w == expected) return w;
        tick();
    }
    fail("overview did not settle");
    return None;
}
static void focus_is(Window expected) {
    for (unsigned i = 0; i < 300; i++) {
        Window focused; int revert;
        XGetInputFocus(display, &focused, &revert);
        if (focused == expected && property(root, "_NET_ACTIVE_WINDOW", XA_WINDOW) == expected) return;
        tick();
    }
    Window actual; int revert;
    XGetInputFocus(display, &actual, &revert);
    fprintf(stderr, "expected focus 0x%lx actual=0x%lx active=0x%lx\n", expected, actual,
            property(root, "_NET_ACTIVE_WINDOW", XA_WINDOW));
    fail("X input focus differs from EWMH active client");
}
static Atom compositor_selection(void) {
    char name[64];
    snprintf(name, sizeof(name), "_NET_WM_CM_S%d", DefaultScreen(display));
    return XInternAtom(display, name, False);
}
static void await_compositor(int present) {
    for (unsigned i = 0; i < 300; i++) {
        if (!!XGetSelectionOwner(display, compositor_selection()) == present) return;
        tick();
    }
    fail("compositor ownership did not settle");
}
static void start_compositor(void) {
    comp_pid = fork();
    assert(comp_pid >= 0);
    if (!comp_pid) {
        close(ConnectionNumber(display));
        execl(comp_path, "dani-comp", "-d", private_display, "--no-fade",
              "--no-shadow", "--dim", "1", (char *)NULL);
        _exit(127);
    }
    await_compositor(1);
}
static void check_layout(Window a, Window b) {
    for (unsigned i = 0; i < 300; i++) {
        XWindowAttributes aa, bb;
        assert(XGetWindowAttributes(display, a, &aa));
        assert(XGetWindowAttributes(display, b, &bb));
        if (aa.map_state == IsViewable && bb.map_state == IsViewable &&
            aa.width > 100 && bb.width > 100 && aa.height > 100 && bb.height > 100 &&
            aa.x + aa.width <= bb.x) return;
        tick();
    }
    fail("tile geometry does not match side-by-side baseline");
}
int main(int argc, char **argv) {
    if ((argc != 2 && argc != 3) || argv[1][0] != ':') {
        fprintf(stderr, "usage: animation-wm :PRIVATE [dani-comp-path]\n");
        return 2;
    }
    private_display = argv[1];
    if (argc == 3) comp_path = argv[2];
    atexit(cleanup);
    display = XOpenDisplay(private_display);
    assert(display);
    XSetErrorHandler(xerror);
    root = DefaultRootWindow(display);
    time_window = XCreateSimpleWindow(display, root, 0, 0, 1, 1, 0, 0, 0);
    XSelectInput(display, time_window, PropertyChangeMask);
    await_property(root, "_NET_CURRENT_DESKTOP", XA_CARDINAL, 0);
    /* EWMH is published before startup finishes grabs/bar setup. */
    for (unsigned i = 0; i < 50; i++) tick();
    if (comp_path) start_compositor();
    /* Keep the pointer off the client areas to avoid incidental hover focus. */
    XWarpPointer(display, None, root, 0, 0, 0, 0, 1, 1);
    Window a = XCreateSimpleWindow(display, root, 50, 50, 200, 120, 0, 0, 0x336699);
    Window b = XCreateSimpleWindow(display, root, 60, 60, 200, 120, 0, 0, 0x993366);
    XMapWindow(display, a); XFlush(display); await_mapping(a, 1);
    XMapWindow(display, b); XFlush(display); await_mapping(b, 1);
    focus_is(b); check_layout(a, b);
    key(root, XK_2, Mod4Mask);
    await_property(root, "_NET_CURRENT_DESKTOP", XA_CARDINAL, 1);
    await_mapping(a, 0); await_mapping(b, 0);
    await_property(a, "_NET_WM_DESKTOP", XA_CARDINAL, 0);
    await_property(b, "_NET_WM_DESKTOP", XA_CARDINAL, 0);
    key(root, XK_1, Mod4Mask);
    await_property(root, "_NET_CURRENT_DESKTOP", XA_CARDINAL, 0);
    await_mapping(a, 1); await_mapping(b, 1); focus_is(b); check_layout(a, b);
    key(root, XK_o, Mod4Mask);
    Window overlay = await_overview(1);
    key(overlay, XK_Right, 0);
    key(overlay, XK_Escape, 0);
    await_overview(0);
    await_property(root, "_NET_CURRENT_DESKTOP", XA_CARDINAL, 0);
    focus_is(b);
    if (XGrabKeyboard(display, root, False, GrabModeAsync, GrabModeAsync, CurrentTime) != GrabSuccess)
        fail("overview leaked keyboard grab");
    XUngrabKeyboard(display, CurrentTime); XSync(display, False);
    if (comp_path) {
        /* Stop and restart only our private child, never a session compositor. */
        cleanup(); await_compositor(0);
        focus_is(b); check_layout(a, b);
        start_compositor(); focus_is(b); check_layout(a, b);
    }
    XDestroyWindow(display, b); XFlush(display);
    focus_is(a); await_mapping(a, 1);
    XDestroyWindow(display, a); XSync(display, False);
    cleanup();
    XCloseDisplay(display);
    printf("METRIC: tolerated root-query destroy races=%u\n", query_races);
    printf("PASS: private WM tile/workspace/focus/EWMH/overview-cancel/ungrab/client-removal baseline (%s)\n",
           comp_path ? "compositor stop/restart" : "no compositor");
    return 0;
}
