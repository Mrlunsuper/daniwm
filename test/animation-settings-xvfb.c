/* Phase 1A end-to-end WM launch/reload on an explicitly supplied private server. */
#define _POSIX_C_SOURCE 200809L
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include "../src/animation-settings.h"
#include "../src/animation-lifecycle.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static Display *d;
static Window root, stamp;
static Atom settings;
static const char *config_path;
static void tick(void) {
    struct timespec t = {0, 10000000};
    nanosleep(&t, NULL);
}
static void config(int compositor, int enabled, int reduced, int ms) {
    FILE *f = fopen(config_path, "w"); assert(f);
    fprintf(f, "compositor = %d\nanimations = %d\nreduce_motion = %d\nanimation_open_ms = %d\n"
        "animation_close_ms = 1000\n"
        "bind = mod+Shift+r:reload_config\nbind = mod+c:compositor\n"
        "bind = mod+1:ws 1\nbind = mod+2:ws 2\nbind = mod+m:monocle\nbind = mod+t:tile\n", 
        compositor, enabled, reduced, ms);
    assert(fclose(f) == 0);
}
static void action(KeySym sym, unsigned modifiers) {
    Atom atom = XInternAtom(d, "_DANIWM_TEST_TIME", False);
    unsigned char value = 1;
    XChangeProperty(d, stamp, atom, XA_INTEGER, 8, PropModeReplace, &value, 1);
    XEvent e;
    XWindowEvent(d, stamp, PropertyChangeMask, &e);
    Time time = e.xproperty.time;
    e = (XEvent){0};
    e.xkey.type = KeyPress; e.xkey.display = d; e.xkey.window = root;
    e.xkey.root = root; e.xkey.same_screen = True;
    e.xkey.keycode = XKeysymToKeycode(d, sym); e.xkey.state = modifiers; e.xkey.time = time;
    assert(XSendEvent(d, root, False, KeyPressMask, &e));
    XFlush(d);
}
static void await_settings(int enabled, int reduced, int ms) {
    for (unsigned i = 0; i < 300; i++) {
        Atom type; int format; unsigned long n, extra;
        unsigned char *data = NULL;
        int ok = XGetWindowProperty(d, root, settings, 0, 5, False, XA_CARDINAL,
            &type, &format, &n, &extra, &data) == Success && type == XA_CARDINAL &&
            format == 32 && n == 5 && !extra && data;
        if (ok) {
            unsigned long *v = (unsigned long *)data;
            ok = animation_settings_valid(v, n, extra, v[1]) &&
                v[2] == (unsigned long)enabled && v[3] == (unsigned long)reduced &&
                v[4] == (unsigned long)ms;
        }
        if (data) XFree(data);
        if (ok) return;
        tick();
    }
    fprintf(stderr, "FAIL: WM did not publish expected settings %d/%d/%d\n", enabled, reduced, ms);
    exit(1);
}
static Window compositor(void) {
    char atom[64];
    snprintf(atom, sizeof(atom), "_NET_WM_CM_S%d", DefaultScreen(d));
    return XGetSelectionOwner(d, XInternAtom(d, atom, False));
}
static unsigned long pixel(int x, int y) {
    XImage *image = XGetImage(d, root, x, y, 1, 1, AllPlanes, ZPixmap);
    assert(image);
    unsigned long value = XGetPixel(image, 0, 0);
    XDestroyImage(image);
    return value;
}
int main(int argc, char **argv) {
    assert(argc == 3 && argv[1][0] == ':');
    config_path = argv[2];
    d = XOpenDisplay(argv[1]); assert(d);
    root = DefaultRootWindow(d);
    settings = XInternAtom(d, ANIMATION_SETTINGS_ATOM, False);
    stamp = XCreateSimpleWindow(d, root, 0, 0, 1, 1, 0, 0, 0);
    XSelectInput(d, stamp, PropertyChangeMask);
    await_settings(1, 0, 1000);
    Window owner = None;
    for (unsigned i = 0; i < 300 && !owner; i++) { owner = compositor(); if (!owner) tick(); }
    assert(owner);
    /* Allow startup grabs, not just the selection announcement, to finish. */
    for (unsigned i = 0; i < 50; i++) tick();
    Window window = XCreateSimpleWindow(d, root, 40, 40, 200, 120, 0, 0, 0xffffff);
    XMapWindow(d, window); XFlush(d);
    XWindowAttributes a;
    int viewable = 0;
    for (unsigned i = 0; i < 300; i++) {
        assert(XGetWindowAttributes(d, window, &a));
        if (a.map_state == IsViewable) { viewable = 1; break; }
        tick();
    }
    assert(viewable);
    /* Wait a few frames so captured root is compositor output, not the
     * frame preceding map. A 1-second duration leaves ample headroom. */
    for (unsigned i = 0; i < 10; i++) tick();
    unsigned long partial = pixel(a.x + a.width / 2, a.y + a.height / 2);
    assert((partial & 255) < 230);
    config(1, 1, 1, 1000); action(XK_r, Mod4Mask | ShiftMask);
    await_settings(1, 1, 1000);
    int complete = 0;
    for (unsigned i = 0; i < 100; i++) {
        if (pixel(a.x + a.width / 2, a.y + a.height / 2) == 0xffffff) { complete = 1; break; }
        tick();
    }
    assert(complete && compositor() == owner);
    Window focused; int revert;
    XGetInputFocus(d, &focused, &revert); assert(focused == window);
    config(1, 0, 0, 140); action(XK_r, Mod4Mask | ShiftMask); await_settings(0, 0, 140);
    assert(compositor() == owner);
    config(1, 1, 0, 0); action(XK_r, Mod4Mask | ShiftMask); await_settings(1, 0, 0);
    assert(compositor() == owner);
    /* Phase 1B: logical workspace hide must never produce a closing image.
     * Observe ordered reason -> UnmapNotify on an independent connection. */
    config(1, 1, 0, 0); action(XK_r, Mod4Mask | ShiftMask); await_settings(1, 0, 0);
    for (unsigned i = 0; i < 10; i++) tick();
    XSelectInput(d, root, SubstructureNotifyMask);
    while (XPending(d)) { XEvent event; XNextEvent(d, &event); }
    action(XK_2, Mod4Mask);
    int saw_reason = 0, saw_unmap = 0;
    Atom hide = XInternAtom(d, ANIMATION_REASON_ATOM, False);
    for (unsigned i = 0; i < 200 && !saw_unmap; i++) {
        while (XPending(d)) {
            XEvent event; XNextEvent(d, &event);
            if (event.type == ClientMessage && event.xclient.message_type == hide &&
                (Window)event.xclient.data.l[2] == window) {
                assert(event.xclient.data.l[4] == ANIMATION_HIDE_WORKSPACE);
                saw_reason = 1;
            }
            if (event.type == UnmapNotify && event.xunmap.window == window) {
                assert(saw_reason); saw_unmap = 1;
            }
        }
        tick();
    }
    assert(saw_unmap);
    for (unsigned i = 0; i < 5; i++) tick();
    assert(pixel(a.x + a.width / 2, a.y + a.height / 2) != 0xffffff);
    action(XK_1, Mod4Mask);
    for (unsigned i = 0; i < 15; i++) tick();
    assert(pixel(a.x + a.width / 2, a.y + a.height / 2) == 0xffffff);
    /* Monocle hides are also instant, including with a full-duration close
     * policy. The old white tile must not overlay the newly selected green. */
    Window other = XCreateSimpleWindow(d, root, 60, 60, 200, 120, 0, 0, 0x004400);
    XMapWindow(d, other); XFlush(d);
    for (unsigned i = 0; i < 20; i++) tick();
    action(XK_m, Mod4Mask);
    for (unsigned i = 0; i < 15; i++) tick();
    assert(XGetWindowAttributes(d, window, &a) && a.map_state == IsUnmapped);
    XWindowAttributes other_a;
    assert(XGetWindowAttributes(d, other, &other_a) && other_a.map_state == IsViewable);
    assert(pixel(other_a.x + 20, other_a.y + other_a.height / 2) == 0x004400);
    XGetInputFocus(d, &focused, &revert); assert(focused == other);
    action(XK_t, Mod4Mask);
    for (unsigned i = 0; i < 10; i++) tick();
    XDestroyWindow(d, other); XSync(d, False);
    /* Let its short-lived visual finish before measuring the white close. */
    for (unsigned i = 0; i < 110; i++) tick();
    assert(XGetWindowAttributes(d, window, &a) && a.map_state == IsViewable);
    /* Destruction is immediate in both client list/focus; pixels alone fade. */
    Window old_window = window;
    XDestroyWindow(d, window); XSync(d, False); window = None;
    int faded = 0;
    for (unsigned i = 0; i < 80; i++) {
        unsigned long value = pixel(a.x + a.width / 2, a.y + a.height / 2) & 255;
        if (value > 20 && value < 230) { faded = 1; break; }
        tick();
    }
    assert(faded);
    XGetInputFocus(d, &focused, &revert); assert(focused != old_window && focused != None);
    config(1, 1, 1, 0); action(XK_r, Mod4Mask | ShiftMask); await_settings(1, 1, 0);
    for (unsigned i = 0; i < 10; i++) tick();
    assert(pixel(a.x + a.width / 2, a.y + a.height / 2) < 0xffffff);
    puts("PASS: Phase 1B ordered workspace/monocle hides stay instant, real close fades, reduced-motion cancels");
    config(0, 1, 0, 140); action(XK_r, Mod4Mask | ShiftMask); await_settings(0, 0, 140);
    assert(compositor() == owner); /* reload never kills the running owner */
    action(XK_c, Mod4Mask);
    int stopped = 0;
    for (unsigned i = 0; i < 300; i++) { if (!compositor()) { stopped = 1; break; } tick(); }
    assert(stopped);
    /* An explicit toggle still starts an instant compositor when auto-start is off. */
    action(XK_c, Mod4Mask);
    Window restarted = None;
    for (unsigned i = 0; i < 300 && !restarted; i++) { restarted = compositor(); if (!restarted) tick(); }
    assert(restarted);
    action(XK_c, Mod4Mask);
    for (unsigned i = 0; i < 300 && compositor(); i++) tick();
    assert(!compositor());
    char selection_name[64];
    snprintf(selection_name, sizeof(selection_name), "_NET_WM_CM_S%d", DefaultScreen(d));
    Atom selection = XInternAtom(d, selection_name, False);
    XSetSelectionOwner(d, selection, stamp, CurrentTime);
    XSync(d, False); assert(compositor() == stamp);
    action(XK_c, Mod4Mask);
    for (unsigned i = 0; i < 20; i++) tick();
    assert(compositor() == stamp); /* foreign owner has no compatible control marker */
    XSetSelectionOwner(d, selection, None, CurrentTime);
    if (window) XDestroyWindow(d, window);
    XDestroyWindow(d, stamp);
    XCloseDisplay(d);
    puts("PASS: WM-managed startup, visible fade, reduced-motion cancellation, same-owner reload, focus, zero/disabled policy and scoped toggle");
    return 0;
}
