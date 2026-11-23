/* Private-server smoke driver: synthetic events exercise the real WM handlers. */
#define _POSIX_C_SOURCE 200809L
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static Display *d;
static Window root;
static int root_query;
static int xerror(Display *display, XErrorEvent *e) {
    /* XQueryTree's list may contain the just-destroyed overlay. The
     * following attribute query then fails; polling must retry safely. */
    if (root_query && e->error_code == BadWindow && e->request_code == 3) return 0;
    char message[128];
    XGetErrorText(display, e->error_code, message, sizeof(message));
    fprintf(stderr, "overview Xvfb: unexpected X error %s req=%u/%u\n",
            message, e->request_code, e->minor_code);
    exit(1);
}
static void pause_tick(void) {
    struct timespec t = {0, 20000000};
    nanosleep(&t, NULL);
}
static void fail(const char *s) {
    fprintf(stderr, "overview Xvfb: %s\n", s);
    exit(1);
}
static int desktop(unsigned long *value) {
    Atom type; int format; unsigned long n, rest; unsigned char *data = NULL;
    int ok = XGetWindowProperty(d, root,
        XInternAtom(d, "_NET_CURRENT_DESKTOP", False), 0, 1, False,
        XA_CARDINAL, &type, &format, &n, &rest, &data) == Success &&
        type == XA_CARDINAL && format == 32 && n == 1;
    if (ok) *value = *(unsigned long *)data;
    if (data) XFree(data);
    return ok;
}
static Window overlay(void) {
    Window rr, parent, *children = NULL, found = None; unsigned n = 0;
    if (!XQueryTree(d, root, &rr, &parent, &children, &n))
        fail("cannot query root tree");
    for (unsigned i = 0; i < n; i++) {
        XWindowAttributes a;
        root_query = 1;
        int valid = XGetWindowAttributes(d, children[i], &a);
        XSync(d, False);
        root_query = 0;
        if (valid && a.override_redirect &&
            a.map_state == IsViewable && a.class == InputOutput &&
            a.width == DisplayWidth(d, DefaultScreen(d)) &&
            a.height == DisplayHeight(d, DefaultScreen(d)))
            found = children[i];
    }
    if (children) XFree(children);
    return found;
}
static void key(Window window, KeySym sym, unsigned state) {
    XEvent e = {0};
    e.xkey.type = KeyPress; e.xkey.display = d; e.xkey.window = window;
    e.xkey.root = root; e.xkey.same_screen = True;
    e.xkey.time = CurrentTime; e.xkey.state = state;
    e.xkey.keycode = XKeysymToKeycode(d, sym);
    if (!e.xkey.keycode || !XSendEvent(d, window, False, KeyPressMask, &e))
        fail("cannot send key event");
    XFlush(d);
}
int main(void) {
    unsigned long original = 0, current = 0;
    /* Retry only the explicitly supplied private DISPLAY. */
    for (int i = 0; i < 250 && !d; i++) {
        d = XOpenDisplay(NULL);
        if (!d) pause_tick();
    }
    if (!d) fail("cannot connect to private display");
    XSetErrorHandler(xerror);
    root = DefaultRootWindow(d);
    int ready = 0;
    for (int i = 0; i < 250; i++) {
        if (desktop(&original)) { ready = 1; break; }
        pause_tick();
    }
    if (!ready) fail("WM did not publish current desktop");
    /* Startup publishes EWMH before finishing grabs/bar setup. */
    for (int i = 0; i < 25; i++) pause_tick();
    if (overlay()) fail("unexpected full-screen overlay before toggle");
    key(root, XK_o, Mod4Mask);
    Window shown = None;
    for (int i = 0; i < 250; i++) {
        shown = overlay();
        if (shown) break;
        pause_tick();
    }
    if (!shown) fail("Super+o did not map overview overlay");
    if (!desktop(&current) || current != original)
        fail("opening overview changed desktop");
    key(shown, XK_Escape, 0);
    int closed = 0;
    for (int i = 0; i < 250; i++) {
        if (!overlay()) { closed = 1; break; }
        pause_tick();
    }
    if (!closed) fail("Escape did not close overview");
    if (!desktop(&current) || current != original)
        fail("cancelling overview changed desktop");
    XCloseDisplay(d);
    puts("PASS: private Xvfb overview show / Escape / unchanged desktop");
    return 0;
}
