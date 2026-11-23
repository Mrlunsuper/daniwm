/* Phase 2 best-effort layout movement under WM-managed dani-comp. Private display only. */
#define _POSIX_C_SOURCE 200809L
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <xcb/xcb.h>
#include <xcb/res.h>
#include <X11/Xlib-xcb.h>

#define COLOR_A 0xcc0000UL
#define COLOR_B 0x0000ccUL

static Display *d;
static Window root, stamp;
static const char *config_path;
static int failures;

static void tick(void) {
    struct timespec t = {0, 10000000};
    nanosleep(&t, NULL);
}
static void wait_ms(unsigned ms) { for (unsigned i = 0; i < ms / 10; i++) tick(); }
static void check(int ok, const char *what) {
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}
static void config(int layout_ms) {
    FILE *f = fopen(config_path, "w"); assert(f);
    fprintf(f, "compositor = 1\nanimations = 1\nreduce_motion = 0\n"
        "animation_open_ms = 0\nanimation_close_ms = 0\nanimation_layout_ms = %d\n"
        "nmaster = 2\nbar_on = 0\ngaps_on = 0\nborder = 0\nfocus = click\n"
        "bind = mod+Shift+r:reload_config\nbind = mod+k:move_up\nbind = mod+j:move_down\n",
        layout_ms);
    assert(fclose(f) == 0);
}
static Time server_time(void) {
    Atom atom = XInternAtom(d, "_DANIWM_TEST_TIME", False);
    unsigned char value = 1;
    XChangeProperty(d, stamp, atom, XA_INTEGER, 8, PropModeReplace, &value, 1);
    XEvent e;
    XWindowEvent(d, stamp, PropertyChangeMask, &e);
    return e.xproperty.time;
}
static void action(KeySym sym, unsigned modifiers) {
    XEvent e = {0};
    Time time = server_time();
    e.xkey.type = KeyPress; e.xkey.display = d; e.xkey.window = root;
    e.xkey.root = root; e.xkey.same_screen = True;
    e.xkey.keycode = XKeysymToKeycode(d, sym); e.xkey.state = modifiers; e.xkey.time = time;
    assert(XSendEvent(d, root, False, KeyPressMask, &e));
    XFlush(d);
}
/* The WM selects SubstructureRedirect on root, so it receives these. */
static void button(int type, Window subwindow, int x, int y) {
    XEvent e = {0};
    Time time = server_time();
    e.xbutton.type = type; e.xbutton.display = d; e.xbutton.window = root;
    e.xbutton.root = root; e.xbutton.subwindow = subwindow; e.xbutton.same_screen = True;
    e.xbutton.x = e.xbutton.x_root = x; e.xbutton.y = e.xbutton.y_root = y;
    e.xbutton.button = Button1; e.xbutton.state = Mod4Mask; e.xbutton.time = time;
    assert(XSendEvent(d, root, False, SubstructureRedirectMask, &e));
    XFlush(d);
}
static unsigned long pixel(int x, int y) {
    XImage *image = XGetImage(d, root, x, y, 1, 1, AllPlanes, ZPixmap);
    assert(image);
    unsigned long value = XGetPixel(image, 0, 0) & 0xffffff;
    XDestroyImage(image);
    return value;
}
static XWindowAttributes geometry(Window w) {
    XWindowAttributes a;
    assert(XGetWindowAttributes(d, w, &a));
    return a;
}
static Window compositor(void) {
    char atom[64];
    snprintf(atom, sizeof(atom), "_NET_WM_CM_S%d", DefaultScreen(d));
    return XGetSelectionOwner(d, XInternAtom(d, atom, False));
}
static void resources(Window anchor, uint64_t *count, uint64_t *bytes) {
    xcb_connection_t *c = XGetXCBConnection(d);
    XSync(d, False);
    xcb_res_query_client_resources_reply_t *r = xcb_res_query_client_resources_reply(c,
        xcb_res_query_client_resources(c, (uint32_t)anchor), NULL);
    assert(r);
    *count = 0;
    for (xcb_res_type_iterator_t it = xcb_res_query_client_resources_types_iterator(r);
         it.rem; xcb_res_type_next(&it)) *count += it.data->count;
    free(r);
    xcb_res_query_client_pixmap_bytes_reply_t *p = xcb_res_query_client_pixmap_bytes_reply(c,
        xcb_res_query_client_pixmap_bytes(c, (uint32_t)anchor), NULL);
    assert(p);
    *bytes = ((uint64_t)p->bytes_overflow << 32) | p->bytes;
    free(p);
}
static void drain(void) {
    while (XPending(d)) { XEvent e; XNextEvent(d, &e); }
}
static unsigned configures(Window w) {
    unsigned n = 0;
    XEvent e;
    while (XCheckTypedWindowEvent(d, w, ConfigureNotify, &e)) n++;
    return n;
}
static Window top_of(Window a, Window b) { return geometry(a).y < geometry(b).y ? a : b; }
/* Swap the focused window with its neighbour. */
static void swap(Window a, Window b) {
    Window focused; int revert;
    XGetInputFocus(d, &focused, &revert);
    assert(focused == a || focused == b);
    action(top_of(a, b) == focused ? XK_j : XK_k, Mod4Mask);
}
/* Swap, then sample the top slot after a delay. */
static unsigned long swap_and_sample(Window a, Window b, unsigned delay_ms) {
    swap(a, b);
    wait_ms(delay_ms);
    return pixel(512, 192);
}
/* Mid-transit, neither tile covers row 10. Instant or frozen pictures never
 * show a gap there, so a gap proves the pictures move. */
static int transit_gap(unsigned ms) {
    for (unsigned i = 0; i < ms / 10; i++) {
        unsigned long p = pixel(512, 10);
        if (p != COLOR_A && p != COLOR_B) return 1;
        tick();
    }
    return 0;
}
static Window map_client(unsigned long color) {
    Window w = XCreateSimpleWindow(d, root, 10, 10, 200, 120, 0, 0, color);
    XMapWindow(d, w); XFlush(d);
    for (unsigned i = 0; i < 300; i++) {
        if (geometry(w).map_state == IsViewable) return w;
        tick();
    }
    fprintf(stderr, "FAIL: client did not map\n");
    exit(1);
}
int main(int argc, char **argv) {
    assert(argc == 3 && argv[1][0] == ':');
    config_path = argv[2];
    d = XOpenDisplay(argv[1]); assert(d);
    root = DefaultRootWindow(d);
    stamp = XCreateSimpleWindow(d, root, 0, 0, 1, 1, 0, 0, 0);
    XSelectInput(d, stamp, PropertyChangeMask);
    Window owner = None;
    for (unsigned i = 0; i < 300 && !owner; i++) { owner = compositor(); if (!owner) tick(); }
    assert(owner);
    wait_ms(500);
    Window a = map_client(COLOR_A), b = map_client(COLOR_B);
    wait_ms(300);
    XWindowAttributes ga = geometry(a), gb = geometry(b);
    assert(ga.width == gb.width && ga.height == gb.height && ga.x == gb.x && ga.y != gb.y);
    assert(pixel(512, 192) == (top_of(a, b) == a ? COLOR_A : COLOR_B));

    /* 1. One swap: real geometry is final at once; the picture moves. */
    XSelectInput(d, a, StructureNotifyMask); XSelectInput(d, b, StructureNotifyMask);
    XSync(d, False); drain();
    Window old_top = top_of(a, b);
    unsigned long early = swap_and_sample(a, b, 60);
    Window new_top = top_of(a, b);
    check(new_top != old_top, "swap commits the final X geometry immediately");
    check(early == (old_top == a ? COLOR_A : COLOR_B),
          "first swap: top slot still shows the old picture after 60 ms");
    check(transit_gap(500), "first swap: pictures pass between the slots (movement visible)");
    wait_ms(900);
    check(pixel(512, 192) == (new_top == a ? COLOR_A : COLOR_B),
          "first swap: final picture matches final geometry after settle");
    unsigned ca = configures(a), cb = configures(b);
    printf("METRIC: ConfigureNotify per client for one 1000 ms swap: a=%u b=%u\n", ca, cb);
    check(ca >= 1 && ca <= 2 && cb >= 1 && cb <= 2, "no per-frame client resize/move storm");

    /* 2. Swap back: windows listed in the previous transaction must animate again. */
    old_top = new_top;
    early = swap_and_sample(a, b, 60);
    new_top = top_of(a, b);
    check(new_top != old_top, "swap back commits the final X geometry immediately");
    check(early == (old_top == a ? COLOR_A : COLOR_B),
          "second swap: top slot still shows the old picture after 60 ms");
    check(transit_gap(500), "second swap: pictures pass between the slots (movement visible)");
    wait_ms(900);
    check(pixel(512, 192) == (new_top == a ? COLOR_A : COLOR_B),
          "second swap: final picture matches final geometry after settle");

    /* 3. Direct mouse drag of a window from the last transaction stays instant. */
    Window dragged = new_top;
    Window other = dragged == a ? b : a;
    XWindowAttributes g0 = geometry(dragged), g = g0;
    int px = g.x + g.width / 2, py = g.y + g.height / 2;
    XWarpPointer(d, None, root, 0, 0, 0, 0, px, py); XSync(d, False);
    button(ButtonPress, dragged, px, py);
    wait_ms(100);
    XWarpPointer(d, None, root, 0, 0, 0, 0, px + 80, py + 80); XSync(d, False);
    int moved = 0;
    for (unsigned i = 0; i < 100 && !moved; i++) {
        g = geometry(dragged);
        moved = g.x != g0.x || g.y != g0.y;
        if (!moved) tick();
    }
    wait_ms(60);
    g = geometry(dragged);
    unsigned long drag_pixel = pixel(g.x + g.width / 2, g.y + g.height / 2);
    printf("METRIC: dragged window moved=%d from %d,%d to %d,%d\n", moved, g0.x, g0.y, g.x, g.y);
    check(moved && drag_pixel == (dragged == a ? COLOR_A : COLOR_B),
          "mouse drag: picture follows real geometry within 60 ms (no stale animation)");
    /* Drop onto the other tile: the WM swaps and re-tiles with animation suppressed. */
    XWindowAttributes go = geometry(other);
    int qx = go.x + go.width / 2, qy = go.y + go.height / 2;
    XWarpPointer(d, None, root, 0, 0, 0, 0, qx, qy); XSync(d, False);
    wait_ms(50);
    button(ButtonRelease, dragged, qx, qy);
    for (unsigned i = 0; i < 100 && (geometry(dragged).y != go.y || geometry(other).y != g0.y); i++) tick();
    check(geometry(dragged).y == go.y && geometry(other).y == g0.y,
          "mouse drop swaps the two tiles");
    wait_ms(60);
    check(pixel(512, 192) == (top_of(a, b) == a ? COLOR_A : COLOR_B) &&
          pixel(512, 576) == (top_of(a, b) == a ? COLOR_B : COLOR_A),
          "mouse drop: pictures match final geometry within 60 ms (no stale animation)");
    XWarpPointer(d, None, root, 0, 0, 0, 0, 1, 1); XSync(d, False);
    wait_ms(1200);

    /* 4. Repeated short swaps settle back to the same retained resources. */
    config(100); action(XK_r, Mod4Mask | ShiftMask); wait_ms(600);
    uint64_t before_count, before_bytes, after_count, after_bytes;
    resources(owner, &before_count, &before_bytes);
    unsigned swaps = 0;
    for (unsigned i = 0; i < 20; i++) {
        Window before = top_of(a, b);
        swap(a, b);
        for (unsigned j = 0; j < 10 && top_of(a, b) == before; j++) tick();
        if (top_of(a, b) != before) swaps++;
        wait_ms(20);
    }
    wait_ms(600);
    check(swaps == 20, "20 rapid swaps each change the real geometry");
    resources(owner, &after_count, &after_bytes);
    printf("METRIC: dani-comp XRes resources=%llu/%llu pixmap_bytes=%llu/%llu after 20 rapid swaps\n",
           (unsigned long long)before_count, (unsigned long long)after_count,
           (unsigned long long)before_bytes, (unsigned long long)after_bytes);
    check(after_count <= before_count && after_bytes <= before_bytes,
          "retained layout pictures are released after rapid swaps settle");

    /* 5. Disabled path: duration 0 makes swaps instant. */
    config(0); action(XK_r, Mod4Mask | ShiftMask); wait_ms(300);
    old_top = top_of(a, b);
    early = swap_and_sample(a, b, 60);
    new_top = top_of(a, b);
    check(new_top != old_top && early == (new_top == a ? COLOR_A : COLOR_B),
          "animation_layout_ms = 0 keeps swaps instant");

    XDestroyWindow(d, a); XDestroyWindow(d, b); XDestroyWindow(d, stamp);
    XCloseDisplay(d);
    return failures ? 1 : 0;
}
