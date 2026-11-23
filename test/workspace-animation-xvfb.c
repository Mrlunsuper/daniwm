/* Phase 3-I workspace crossfade under real daniwm and WM-managed dani-comp.
 * Private display only. */
#define _POSIX_C_SOURCE 200809L
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <assert.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <xcb/xcb.h>
#include <xcb/res.h>
#include <X11/Xlib-xcb.h>

#define RED 0xcc0000UL
#define BLUE 0x0000ccUL
#define GREEN 0x00cc00UL
#define FADE_MS 250
#define CX 512
#define CY 384

static Display *d;
static Window root, stamp;
static const char *config_path;
static int failures;
static unsigned long background;
static Atom A_DESKTOP, A_TXN, A_EPOCH, A_STATE, A_FULLSCREEN;
static int (*fake_motion)(Display *, int, int, int, unsigned long);
static int (*fake_button)(Display *, unsigned int, Bool, unsigned long);
static int (*fake_key)(Display *, unsigned int, Bool, unsigned long);

static long long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static void sleep_ms(unsigned ms) {
    struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&t, NULL);
}
static void check(int ok, const char *what) {
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}
static void config(int workspace_ms, int close_ms, int reduce) {
    FILE *f = fopen(config_path, "w"); assert(f);
    fprintf(f, "compositor = 1\nanimations = 1\nreduce_motion = %d\n"
        "animation_open_ms = 0\nanimation_close_ms = %d\nanimation_layout_ms = 0\n"
        "animation_workspace_ms = %d\nbar_on = 0\ngaps_on = 0\nborder = 0\nfocus = click\n"
        "bind = mod+Shift+r:reload_config\nbind = mod+m:monocle\nbind = mod+t:tile\n"
        "bind = mod+o:overview_toggle\n", reduce, close_ms, workspace_ms);
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
static void key(KeySym sym, unsigned modifiers) {
    XEvent e = {0};
    Time time = server_time();
    e.xkey.type = KeyPress; e.xkey.display = d; e.xkey.window = root;
    e.xkey.root = root; e.xkey.same_screen = True;
    e.xkey.keycode = XKeysymToKeycode(d, sym); e.xkey.state = modifiers; e.xkey.time = time;
    assert(XSendEvent(d, root, False, KeyPressMask, &e));
    XFlush(d);
}
static void reload(int workspace_ms, int close_ms, int reduce) {
    config(workspace_ms, close_ms, reduce);
    key(XK_r, Mod4Mask | ShiftMask);
    sleep_ms(400);
}
static void client_message(Window w, Atom type, long l0, long l1, long l2) {
    XEvent e = {0};
    e.xclient.type = ClientMessage; e.xclient.window = w;
    e.xclient.message_type = type; e.xclient.format = 32;
    e.xclient.data.l[0] = l0; e.xclient.data.l[1] = l1; e.xclient.data.l[2] = l2;
    assert(XSendEvent(d, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &e));
    XFlush(d);
}
static long cardinal(Atom atom, long index) {
    Atom type; int format; unsigned long n = 0, extra = 0;
    unsigned char *data = NULL;
    long value = -1;
    if (XGetWindowProperty(d, root, atom, 0, 32, False, XA_CARDINAL, &type, &format,
        &n, &extra, &data) == Success && type == XA_CARDINAL && format == 32 &&
        (unsigned long)index < n) value = (long)((unsigned long *)data)[index];
    if (data) XFree(data);
    return value;
}
static long prepare_sequence(void) { return cardinal(A_TXN, 3); }
static long prepare_members(void) { return cardinal(A_TXN, 7); }
static void desktop(int n) { client_message(root, A_DESKTOP, n, (long)server_time(), 0); }
static void desktop_wait(int n) {
    desktop(n);
    for (unsigned i = 0; i < 200 && cardinal(A_DESKTOP, 0) != n; i++) sleep_ms(5);
    assert(cardinal(A_DESKTOP, 0) == n);
}
static unsigned long pixel(int x, int y) {
    XImage *image = XGetImage(d, root, x, y, 1, 1, AllPlanes, ZPixmap);
    assert(image);
    unsigned long value = XGetPixel(image, 0, 0) & 0xffffff;
    XDestroyImage(image);
    return value;
}
static unsigned channel(unsigned long p, int shift) { return (unsigned)(p >> shift) & 0xff; }
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
static void resources(Window anchor, uint64_t *count, uint64_t *bytes, const char *label) {
    xcb_connection_t *c = XGetXCBConnection(d);
    XSync(d, False);
    xcb_res_query_client_resources_reply_t *r = xcb_res_query_client_resources_reply(c,
        xcb_res_query_client_resources(c, (uint32_t)anchor), NULL);
    assert(r);
    *count = 0;
    printf("METRIC: XRes %s", label);
    for (xcb_res_type_iterator_t it = xcb_res_query_client_resources_types_iterator(r);
         it.rem; xcb_res_type_next(&it)) {
        char *name = XGetAtomName(d, it.data->resource_type);
        printf(" %s=%u", name ? name : "?", it.data->count);
        if (name) XFree(name);
        *count += it.data->count;
    }
    Window r0, p0, *kids = NULL;
    unsigned nk = 0;
    if (XQueryTree(d, root, &r0, &p0, &kids, &nk) && kids) XFree(kids);
    printf(" root_children=%u\n", nk);
    /* Composite keeps one record for each child of root while redirected. */
    *count -= nk;
    free(r);
    xcb_res_query_client_pixmap_bytes_reply_t *p = xcb_res_query_client_pixmap_bytes_reply(c,
        xcb_res_query_client_pixmap_bytes(c, (uint32_t)anchor), NULL);
    assert(p);
    *bytes = ((uint64_t)p->bytes_overflow << 32) | p->bytes;
    free(p);
}
static Window map_client(unsigned long color) {
    Window w = XCreateSimpleWindow(d, root, 10, 10, 200, 120, 0, 0, color);
    XSelectInput(d, w, ButtonPressMask);
    XMapWindow(d, w); XFlush(d);
    for (unsigned i = 0; i < 300; i++) {
        if (geometry(w).map_state == IsViewable) { sleep_ms(300); return w; }
        sleep_ms(10);
    }
    fprintf(stderr, "FAIL: client did not map\n");
    exit(1);
}

/* Samples of one pixel from the switch request on. */
typedef struct {
    unsigned n;
    unsigned long p[400];
    long long t[400];
} Trace;
static Trace trace;
static void sample(int x, int y, unsigned ms) {
    long long start = now_ms();
    trace.n = 0;
    while (now_ms() - start < ms && trace.n < 400) {
        trace.p[trace.n] = pixel(x, y);
        trace.t[trace.n++] = now_ms() - start;
        sleep_ms(3);
    }
}
/* A blend is any sample that is neither end colour nor the background. */
static int is_blend(unsigned long p, unsigned long from, unsigned long to) {
    return p != from && p != to && p != background;
}
static unsigned blends(unsigned long from, unsigned long to) {
    unsigned n = 0;
    for (unsigned i = 0; i < trace.n; i++) n += is_blend(trace.p[i], from, to);
    return n;
}
static long first_blend(unsigned long from, unsigned long to) {
    for (unsigned i = 0; i < trace.n; i++) if (is_blend(trace.p[i], from, to)) return (long)i;
    return -1;
}
/* Time of the last sample that is not the final colour. */
static long long settle(unsigned long to) {
    long long last = 0;
    for (unsigned i = 0; i < trace.n; i++) if (trace.p[i] != to) last = trace.t[i];
    return last;
}
static unsigned with_channel(int shift, unsigned min) {
    unsigned n = 0;
    for (unsigned i = 0; i < trace.n; i++) n += channel(trace.p[i], shift) >= min;
    return n;
}
/* One switch request, then sample. */
static void switch_and_sample(int n, int x, int y, unsigned ms) {
    desktop(n);
    sample(x, y, ms);
}
static void report(const char *name, unsigned long from, unsigned long to) {
    long fb = first_blend(from, to);
    printf("METRIC: %s blends=%u/%u first_blend=%06lx@%lldms settle=%lldms final=%06lx\n",
        name, blends(from, to), trace.n, fb >= 0 ? trace.p[fb] : 0UL,
        fb >= 0 ? trace.t[fb] : -1LL, settle(to), trace.n ? trace.p[trace.n - 1] : 0UL);
}
static int crossfade_ok(unsigned long from, unsigned long to) {
    long fb = first_blend(from, to);
    return fb >= 0 && trace.n && trace.p[trace.n - 1] == to && settle(to) <= FADE_MS + 120;
}
static int instant_ok(unsigned long from, unsigned long to) {
    return !blends(from, to) && trace.n && trace.p[trace.n - 1] == to && settle(to) <= 120;
}

int main(int argc, char **argv) {
    assert(argc == 4 && argv[1][0] == ':');
    config_path = argv[2];
    const char *mode = argv[3];
    void *xtst = dlopen("libXtst.so.6", RTLD_NOW | RTLD_LOCAL);
    if (xtst) {
        /* memcpy avoids the object-to-function pointer cast that ISO C forbids. */
        void *mo = dlsym(xtst, "XTestFakeMotionEvent"), *bu = dlsym(xtst, "XTestFakeButtonEvent");
        void *ke = dlsym(xtst, "XTestFakeKeyEvent");
        if (mo && bu && ke) {
            memcpy(&fake_motion, &mo, sizeof mo);
            memcpy(&fake_button, &bu, sizeof bu);
            memcpy(&fake_key, &ke, sizeof ke);
        }
    }
    d = XOpenDisplay(argv[1]); assert(d);
    root = DefaultRootWindow(d);
    A_DESKTOP = XInternAtom(d, "_NET_CURRENT_DESKTOP", False);
    A_TXN = XInternAtom(d, "_DANIWM_WORKSPACE_V1", False);
    A_EPOCH = XInternAtom(d, "_DANIWM_MONITOR_EPOCH_V1", False);
    A_STATE = XInternAtom(d, "_NET_WM_STATE", False);
    A_FULLSCREEN = XInternAtom(d, "_NET_WM_STATE_FULLSCREEN", False);
    stamp = XCreateSimpleWindow(d, root, 0, 0, 1, 1, 0, 0, 0);
    XSelectInput(d, stamp, PropertyChangeMask);
    Window owner = None;
    for (unsigned i = 0; i < 300 && !owner; i++) { owner = compositor(); if (!owner) sleep_ms(10); }
    assert(owner);
    sleep_ms(500);
    background = pixel(CX, CY);
    printf("METRIC: mode=%s background=%06lx xtest=%d\n", mode, background, fake_button != NULL);

    /* Setup: red A on desktop 0, blue B on desktop 1, same rectangle. */
    Window a = map_client(RED);
    desktop_wait(1);
    Window b = map_client(BLUE);
    desktop_wait(0);
    sleep_ms(400);
    XWindowAttributes ga = geometry(a), gb = geometry(b);
    assert(ga.x == gb.x && ga.y == gb.y && ga.width == gb.width && ga.height == gb.height);
    assert(pixel(CX, CY) == RED);
    uint64_t base_count, base_bytes, count, bytes;
    resources(owner, &base_count, &base_bytes, "baseline");

    /* 1. Crossfade 0 -> 1 with open = close = 0. */
    long seq = prepare_sequence();
    switch_and_sample(1, CX, CY, 500);
    report("0->1", RED, BLUE);
    check(prepare_sequence() != seq && prepare_members() == 1, "view() sends one PREPARE with one member");
    check(crossfade_ok(RED, BLUE), "crossfade: blend frames, then the exact incoming frame");
    long fb = first_blend(RED, BLUE);
    check(fb >= 0 && channel(trace.p[fb], 16) > channel(trace.p[fb], 0),
        "early frame: the outgoing image dominates");

    /* 2. Input during the fade goes to the incoming client. */
    if (fake_motion && fake_button) {
        fake_motion(d, -1, CX, CY, 0); XSync(d, False); sleep_ms(100);
        XEvent e;
        while (XCheckMaskEvent(d, ButtonPressMask, &e)) { }
        desktop_wait(0);
        sleep_ms(20);
        unsigned long shown = pixel(CX, CY);
        fake_button(d, 1, True, 0); fake_button(d, 1, False, 0); XSync(d, False);
        Window focused; int revert;
        XGetInputFocus(d, &focused, &revert);
        sleep_ms(400);
        unsigned to_a = 0, to_b = 0;
        while (XCheckMaskEvent(d, ButtonPressMask, &e)) {
            if (e.xbutton.window == a) to_a++;
            if (e.xbutton.window == b) to_b++;
        }
        printf("METRIC: click during fade shown=%06lx incoming=%u outgoing=%u focus=%s\n",
            shown, to_a, to_b, focused == a ? "incoming" : "other");
        check(to_a == 1 && to_b == 0 && focused == a,
            "click during the fade goes to the incoming client");
    } else {
        puts("BLOCKED: input check needs runtime libXtst.so.6");
        desktop_wait(0);
        sleep_ms(400);
    }

    /* 3. Epoch notification mid-fade ends the fade at once. */
    desktop(1);
    long long start = now_ms();
    unsigned long p = RED;
    while (now_ms() - start < 120 && !is_blend(p, RED, BLUE)) p = pixel(CX, CY);
    long long seen = now_ms() - start;
    long epoch = cardinal(A_EPOCH, 0);
    unsigned long same = (unsigned long)epoch;
    XChangeProperty(d, root, A_EPOCH, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&same, 1);
    XFlush(d);
    sleep_ms(40);
    unsigned long after = pixel(CX, CY);
    printf("METRIC: epoch blend=%06lx@%lldms after_cancel=%06lx\n", p, seen, after);
    check(epoch > 0 && is_blend(p, RED, BLUE) && after == BLUE,
        "monitor epoch notification mid-fade: exact final frame at once");
    sleep_ms(300);
    desktop_wait(0);
    sleep_ms(400);

    /* 4. Monocle: only the shown client is a member; the hidden one never appears. */
    Window g = map_client(GREEN);
    key(XK_m, Mod4Mask);
    sleep_ms(300);
    unsigned long shown = pixel(CX, CY);
    seq = prepare_sequence();
    switch_and_sample(1, CX, CY, 500);
    report("monocle 0->1", shown, BLUE);
    unsigned long hidden = shown == GREEN ? RED : GREEN;
    check(prepare_sequence() != seq && prepare_members() == 1,
        "monocle: PREPARE lists only the viewable client");
    check(crossfade_ok(shown, BLUE) &&
        !with_channel(hidden == RED ? 16 : 8, 0x10),
        "monocle: crossfade of the shown client; the hidden client never appears");
    desktop_wait(0);
    key(XK_t, Mod4Mask);
    XDestroyWindow(d, g); XFlush(d);
    sleep_ms(500);

    /* 5. send_to: the moved client is not a member. */
    Window s = map_client(GREEN);
    seq = prepare_sequence();
    key(XK_2, Mod4Mask | ShiftMask);
    sample(40, CY, 500);
    unsigned long final = trace.n ? trace.p[trace.n - 1] : 0;
    report("send_to 0->1", RED, final);
    check(prepare_sequence() != seq && prepare_members() == 1 && cardinal(A_DESKTOP, 0) == 1,
        "send_to: PREPARE lists the outgoing clients only");
    check(final != RED && crossfade_ok(RED, final), "send_to: crossfade, then the exact final frame");
    XDestroyWindow(d, s); XFlush(d);
    sleep_ms(300);
    desktop_wait(0);
    sleep_ms(400);

    /* 6. Fullscreen policy: no PREPARE to or from a fullscreen workspace. */
    client_message(b, A_STATE, 1, (long)A_FULLSCREEN, 0);
    sleep_ms(300);
    seq = prepare_sequence();
    switch_and_sample(1, CX, CY, 300);
    report("fullscreen 0->1", RED, BLUE);
    check(prepare_sequence() == seq && instant_ok(RED, BLUE),
        "fullscreen incoming workspace: no PREPARE, instant switch");
    switch_and_sample(0, CX, CY, 300);
    check(prepare_sequence() == seq && instant_ok(BLUE, RED),
        "fullscreen outgoing workspace: no PREPARE, instant switch");
    client_message(b, A_STATE, 0, (long)A_FULLSCREEN, 0);
    sleep_ms(300);

    /* 7. Duration 0 and reduced motion: no PREPARE, instant. */
    reload(0, 0, 0);
    seq = prepare_sequence();
    switch_and_sample(1, CX, CY, 300);
    check(prepare_sequence() == seq && instant_ok(RED, BLUE),
        "animation_workspace_ms = 0: no PREPARE, instant switch");
    desktop_wait(0);
    reload(FADE_MS, 0, 1);
    seq = prepare_sequence();
    switch_and_sample(1, CX, CY, 300);
    check(prepare_sequence() == seq && instant_ok(RED, BLUE),
        "reduce_motion = 1: no PREPARE, instant switch");
    desktop_wait(0);

    /* 8. A close just before a switch: its image does not follow. */
    reload(FADE_MS, FADE_MS, 0);
    Window c = map_client(GREEN);
    int cx = geometry(c).x + geometry(c).width / 2;
    assert(pixel(cx, CY) == GREEN);
    XDestroyWindow(d, c); XFlush(d);
    sleep_ms(15);
    unsigned long closing = pixel(cx, CY);
    desktop_wait(1);
    sleep_ms(20);
    sample(cx, CY, 300);
    /* A close image that follows the switch would keep green over the new
     * workspace for its whole fade. Allow only presentation lag before the
     * first frame that shows the new workspace. */
    unsigned first = 0, late = 0;
    while (first < trace.n && channel(trace.p[first], 0) < 0x10) first++;
    for (unsigned i = first; i < trace.n; i++) late += channel(trace.p[i], 8) >= 0x10;
    printf("METRIC: close image before switch=%06lx; first switched frame=%lldms green before=%u after=%u\n",
        closing, first < trace.n ? trace.t[first] : -1LL, with_channel(8, 0x10) - late, late);
    check(channel(closing, 8) >= 0x10 && first < trace.n && trace.t[first] <= 100 && !late &&
        trace.p[trace.n - 1] == BLUE,
        "close just before a switch: the close image does not follow");
    desktop_wait(0);
    reload(FADE_MS, 0, 0);
    sleep_ms(300);

    /* 9. Switch while the overview is open, and right after it closes. */
    key(XK_o, Mod4Mask);
    sleep_ms(300);
    seq = prepare_sequence();
    switch_and_sample(1, CX, CY, 500);
    report("overview open 0->1", RED, BLUE);
    check(prepare_sequence() == seq && trace.p[trace.n - 1] == BLUE && settle(BLUE) <= 120,
        "switch with the overview open: no PREPARE, exact final frame at once");
    if (fake_key) {
        /* Real keys go to the overview grab: select desktop 0, then commit. */
        key(XK_o, Mod4Mask);
        sleep_ms(300);
        seq = prepare_sequence();
        unsigned left = XKeysymToKeycode(d, XK_Left), enter = XKeysymToKeycode(d, XK_Return);
        fake_key(d, left, True, 0); fake_key(d, left, False, 0); XSync(d, False);
        sleep_ms(50);
        fake_key(d, enter, True, 0); fake_key(d, enter, False, 0); XSync(d, False);
        sample(CX, CY, 500);
        report("overview commit 1->0", BLUE, RED);
        check(prepare_sequence() == seq && cardinal(A_DESKTOP, 0) == 0 &&
            trace.p[trace.n - 1] == RED && settle(RED) <= 150,
            "overview commit: no PREPARE, exact final frame at once");
    } else {
        puts("BLOCKED: overview commit check needs runtime libXtst.so.6");
        desktop_wait(0);
        sleep_ms(300);
    }
    key(XK_o, Mod4Mask);
    sleep_ms(300);
    key(XK_o, Mod4Mask);
    seq = prepare_sequence();
    switch_and_sample(1, CX, CY, 500);
    report("overview closed 0->1", RED, BLUE);
    check(prepare_sequence() != seq && trace.p[trace.n - 1] == BLUE && settle(BLUE) <= FADE_MS + 120,
        "switch right after the overview closes: exact final frame (crossfade or bypass)");

    /* 10. Storm: 10 switches 25 ms apart. */
    seq = prepare_sequence();
    for (int i = 0; i < 10; i++) { desktop(i % 2 ? 1 : 0); sleep_ms(25); }
    sample(CX, CY, 500);
    long sent = prepare_sequence() - seq;
    report("storm", RED, BLUE);
    check(sent == 10 && trace.p[trace.n - 1] == BLUE && settle(BLUE) <= FADE_MS + 120,
        "switch storm: newest wins, exact final frame");

    /* 11. Three members: every tile fades, then shows the exact final frame. */
    desktop_wait(0);
    sleep_ms(400);
    {
        Window tiles[2];
        tiles[0] = map_client(GREEN);
        tiles[1] = map_client(0xcccc00UL);
        Window all[3] = {a, tiles[0], tiles[1]};
        int px[3], py[3];
        unsigned long from[3];
        for (int i = 0; i < 3; i++) {
            XWindowAttributes t = geometry(all[i]);
            px[i] = t.x + t.width / 2; py[i] = t.y + t.height / 2;
            from[i] = pixel(px[i], py[i]);
        }
        seq = prepare_sequence();
        desktop(1);
        unsigned mixed[3] = {0}, n = 0;
        unsigned long last[3] = {0};
        long long begin = now_ms(), done[3] = {0};
        while (now_ms() - begin < 500) {
            for (int i = 0; i < 3; i++) {
                last[i] = pixel(px[i], py[i]);
                mixed[i] += is_blend(last[i], from[i], BLUE);
                if (last[i] != BLUE) done[i] = now_ms() - begin;
            }
            n++;
            sleep_ms(3);
        }
        printf("METRIC: 3 members blends=%u,%u,%u/%u settle=%lld,%lld,%lldms\n",
            mixed[0], mixed[1], mixed[2], n, done[0], done[1], done[2]);
        int ok = prepare_sequence() != seq && prepare_members() == 3;
        for (int i = 0; i < 3; i++)
            ok = ok && mixed[i] && last[i] == BLUE && done[i] <= FADE_MS + 120;
        check(ok, "3 members: each tile crossfades, then the exact final frame");
        desktop_wait(0);
        XDestroyWindow(d, tiles[0]); XDestroyWindow(d, tiles[1]); XFlush(d);
    }

    /* 12. More than 8 leaving clients (the pool size): no PREPARE. */
    desktop_wait(0);
    sleep_ms(400);
    Window many[8];
    for (int i = 0; i < 8; i++) many[i] = map_client(0x101010UL * (unsigned long)(i + 2));
    seq = prepare_sequence();
    switch_and_sample(1, CX, CY, 300);
    check(prepare_sequence() == seq && trace.p[trace.n - 1] == BLUE && settle(BLUE) <= 120,
        "9 leaving clients: no PREPARE, instant switch");
    for (int i = 0; i < 8; i++) XDestroyWindow(d, many[i]);
    XFlush(d);

    /* 13. Resources return to the baseline. */
    sleep_ms(1500);
    resources(owner, &count, &bytes, "final");
    printf("METRIC: dani-comp XRes resources (less root children)=%llu/%llu pixmap_bytes=%llu/%llu\n",
        (unsigned long long)base_count, (unsigned long long)count,
        (unsigned long long)base_bytes, (unsigned long long)bytes);
    check(count <= base_count && bytes <= base_bytes, "XRes: back to the baseline");

    XDestroyWindow(d, a); XDestroyWindow(d, b); XDestroyWindow(d, stamp);
    XCloseDisplay(d);
    return failures ? 1 : 0;
}
