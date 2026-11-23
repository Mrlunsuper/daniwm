/* Phase 4 overview fade under real daniwm and WM-managed dani-comp.
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
#define BAR 0x00ff00UL
#define FADE_MS 250
#define PX 5   /* overview margin, over the client area */
#define PY 700

static Display *d;
static Window root, stamp;
static const char *config_path;
static int failures;
static Atom A_DESKTOP;
static int with_comp = 1; /* mode nocomp: compositor = 0, highlight rows only */
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
static void config(int overview_ms, int reduce, int animations) {
    FILE *f = fopen(config_path, "w"); assert(f);
    fprintf(f, "compositor = %d\nanimations = %d\nreduce_motion = %d\n"
        "animation_open_ms = 100\nanimation_close_ms = 0\nanimation_layout_ms = 0\n"
        "animation_workspace_ms = 0\nanimation_overview_ms = %d\n"
        "bar_on = 1\nbar_bg = 00ff00\nbar_show_tasks = 0\nbar_modules = clock\ntray = 0\n"
        "gaps_on = 0\nborder = 0\nfocus = click\n"
        "bind = mod+Shift+r:reload_config\nbind = mod+o:overview_toggle\n",
        with_comp, animations, reduce, overview_ms);
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
/* Synthetic key to root: the WM handles it like a bound key. */
static void key(KeySym sym, unsigned modifiers) {
    XEvent e = {0};
    Time time = server_time();
    e.xkey.type = KeyPress; e.xkey.display = d; e.xkey.window = root;
    e.xkey.root = root; e.xkey.same_screen = True;
    e.xkey.keycode = XKeysymToKeycode(d, sym); e.xkey.state = modifiers; e.xkey.time = time;
    assert(XSendEvent(d, root, False, KeyPressMask, &e));
    XFlush(d);
}
/* Real key through XTest: reaches the overview keyboard grab. */
static void tap(KeySym sym) {
    unsigned code = XKeysymToKeycode(d, sym);
    fake_key(d, code, True, 0); fake_key(d, code, False, 0); XSync(d, False);
}
static void reload(int overview_ms, int reduce, int animations) {
    config(overview_ms, reduce, animations);
    key(XK_r, Mod4Mask | ShiftMask);
    sleep_ms(400);
}
static long cardinal(Atom atom) {
    Atom type; int format; unsigned long n = 0, extra = 0;
    unsigned char *data = NULL;
    long value = -1;
    if (XGetWindowProperty(d, root, atom, 0, 1, False, XA_CARDINAL, &type, &format,
        &n, &extra, &data) == Success && type == XA_CARDINAL && format == 32 && n)
        value = (long)((unsigned long *)data)[0];
    if (data) XFree(data);
    return value;
}
static void desktop_wait(int n) {
    XEvent e = {0};
    e.xclient.type = ClientMessage; e.xclient.window = root;
    e.xclient.message_type = A_DESKTOP; e.xclient.format = 32;
    e.xclient.data.l[0] = n; e.xclient.data.l[1] = (long)server_time();
    assert(XSendEvent(d, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &e));
    XFlush(d);
    for (unsigned i = 0; i < 200 && cardinal(A_DESKTOP) != n; i++) sleep_ms(5);
    assert(cardinal(A_DESKTOP) == n);
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
    free(r);
    Window r0, p0, *kids = NULL;
    unsigned nk = 0;
    if (XQueryTree(d, root, &r0, &p0, &kids, &nk) && kids) XFree(kids);
    printf(" root_children=%u\n", nk);
    /* Composite keeps one record for each child of root while redirected. */
    *count -= nk;
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
/* Poll until the keyboard is free; returns the wait in ms, or -1. */
static int keyboard_free(void);
static long wait_keyboard_free(unsigned ms) {
    long long start = now_ms();
    while (now_ms() - start < ms) {
        if (keyboard_free()) return (long)(now_ms() - start);
        sleep_ms(2);
    }
    return -1;
}
static int keyboard_free(void) {
    int r = XGrabKeyboard(d, root, False, GrabModeAsync, GrabModeAsync, CurrentTime);
    if (r == GrabSuccess) XUngrabKeyboard(d, CurrentTime);
    XSync(d, False);
    return r == GrabSuccess;
}

/* Samples of one pixel. */
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
static int is_blend(unsigned long p, unsigned long from, unsigned long to) {
    return p != from && p != to;
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
static long long settle(unsigned long to) {
    long long last = 0;
    for (unsigned i = 0; i < trace.n; i++) if (trace.p[i] != to) last = trace.t[i];
    return last;
}
static unsigned long final(void) { return trace.n ? trace.p[trace.n - 1] : 0; }
static void report(const char *name, unsigned long from, unsigned long to) {
    long fb = first_blend(from, to);
    printf("METRIC: %s blends=%u/%u first_blend=%06lx@%lldms settle=%lldms final=%06lx\n",
        name, blends(from, to), trace.n, fb >= 0 ? trace.p[fb] : 0UL,
        fb >= 0 ? trace.t[fb] : -1LL, settle(to), final());
}
static int fade_ok(unsigned long from, unsigned long to) {
    return first_blend(from, to) >= 0 && final() == to && settle(to) >= FADE_MS / 2 &&
        settle(to) <= FADE_MS + 120;
}
static int instant_ok(unsigned long from, unsigned long to) {
    return !blends(from, to) && final() == to && settle(to) <= 120;
}
/* 4B: a row across the first card row. Returns the count of x that change
 * and, of those, the count that showed a colour between the two ends. */
#define ROW_Y 240
static void row(unsigned long *out) {
    XImage *image = XGetImage(d, root, 0, ROW_Y, 1024, 1, AllPlanes, ZPixmap);
    assert(image);
    for (int x = 0; x < 1024; x++) out[x] = XGetPixel(image, x, 0) & 0xffffff;
    XDestroyImage(image);
}
static void highlight_move(unsigned *changed, unsigned *between) {
    static unsigned long r0[1024], rf[1024], rs[1024];
    static unsigned char mid[1024];
    row(r0);
    memset(mid, 0, sizeof mid);
    tap(XK_Right);
    long long start = now_ms();
    static unsigned long seen[120][1024];
    unsigned n = 0;
    while (now_ms() - start < 400 && n < 120) { row(seen[n++]); sleep_ms(4); }
    row(rf);
    *changed = *between = 0;
    for (int x = 0; x < 1024; x++) {
        if (r0[x] == rf[x]) continue;
        (*changed)++;
        for (unsigned i = 0; i < n; i++) {
            rs[x] = seen[i][x];
            if (rs[x] != r0[x] && rs[x] != rf[x]) { mid[x] = 1; break; }
        }
        *between += mid[x];
    }
}

/* Open the overview and wait for its exact colour at the probe. */
static unsigned long open_overview(unsigned ms) {
    key(XK_o, Mod4Mask);
    sample(PX, PY, ms);
    return final();
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
    if (!fake_key) { puts("BLOCKED: overview animation checks need runtime libXtst.so.6"); return 0; }
    d = XOpenDisplay(argv[1]); assert(d);
    root = DefaultRootWindow(d);
    A_DESKTOP = XInternAtom(d, "_NET_CURRENT_DESKTOP", False);
    stamp = XCreateSimpleWindow(d, root, -20, -20, 1, 1, 0, 0, 0);
    XSelectInput(d, stamp, PropertyChangeMask);
    with_comp = strcmp(mode, "nocomp") != 0;
    Window owner = None;
    for (unsigned i = 0; with_comp && i < 300 && !owner; i++) {
        owner = compositor(); if (!owner) sleep_ms(10);
    }
    assert(!with_comp || owner);
    sleep_ms(500);
    if (!with_comp) {
        /* No compositor: the WM still moves the highlight (4B). */
        assert(!compositor());
        Window c0 = map_client(RED);
        key(XK_o, Mod4Mask);
        sleep_ms(300);
        unsigned changed, between;
        highlight_move(&changed, &between);
        printf("METRIC: nocomp highlight move changed=%u between=%u\n", changed, between);
        check(changed > 0 && between > 0 && cardinal(A_DESKTOP) == 0,
            "no compositor: the highlight still moves through between colours");
        tap(XK_Escape);
        sleep_ms(300);
        reload(0, 0, 1);
        key(XK_o, Mod4Mask);
        sleep_ms(300);
        highlight_move(&changed, &between);
        printf("METRIC: nocomp highlight move (off) changed=%u between=%u\n", changed, between);
        check(changed > 0 && between == 0, "no compositor, duration 0: instant highlight");
        tap(XK_Escape);
        XDestroyWindow(d, c0); XDestroyWindow(d, stamp);
        XCloseDisplay(d);
        return failures ? 1 : 0;
    }

    Window a = map_client(RED);
    desktop_wait(1);
    Window b = map_client(BLUE);
    desktop_wait(0);
    sleep_ms(300);
    assert(pixel(PX, PY) == RED);
    int bx = -1;
    for (int x = 0; x < 1024 && bx < 0; x++) if (pixel(x, 1) == BAR) bx = x;
    assert(bx >= 0);
    fake_motion(d, -1, 512, PY, 0); XSync(d, False);
    printf("METRIC: mode=%s bar probe x=%d\n", mode, bx);

    /* Warm-up open and close, so the baseline holds any lazy WM resources. */
    unsigned long ov = open_overview(500);
    tap(XK_Escape);
    sleep_ms(500);
    printf("METRIC: overview colour=%06lx\n", ov);
    assert(ov != RED && pixel(PX, PY) == RED);
    uint64_t base_count, base_bytes, count, bytes;
    resources(owner, &base_count, &base_bytes, "baseline");

    /* 1. Fade in over the live workspace (not over the background). */
    key(XK_o, Mod4Mask);
    sample(PX, PY, 500);
    report("open", RED, ov);
    long fb = first_blend(RED, ov);
    check(fade_ok(RED, ov), "open: blend frames, then the exact overview frame");
    check(fb >= 0 && channel(trace.p[fb], 16) > channel(trace.p[fb], 8),
        "open: the early frame still shows the workspace below");
    /* Linear alpha over the live workspace crosses over near the middle. A
     * build-up on the last frame (overview taken as an opaque occluder)
     * crosses over within a few frames. */
    long cross = -1;
    for (unsigned i = 0; i < trace.n && cross < 0; i++)
        if (channel(trace.p[i], 8) > channel(trace.p[i], 16)) cross = (long)i;
    long long cross_ms = cross >= 0 && fb >= 0 ? trace.t[cross] - trace.t[fb] : -1;
    printf("METRIC: open crossover %lldms after the first blend\n", cross_ms);
    check(cross_ms >= 70, "open: linear blend over the live workspace (no build-up)");
    check(!keyboard_free(), "open: the WM holds the keyboard grab at once");

    /* 2. Escape: grab released at once; the fading image takes no input. */
    XEvent e;
    while (XCheckMaskEvent(d, ButtonPressMask, &e)) { }
    tap(XK_Escape);
    long freed = wait_keyboard_free(100);
    unsigned long shown = pixel(PX, PY);
    unsigned long bar_shown = pixel(bx, 1);
    fake_button(d, 1, True, 0); fake_button(d, 1, False, 0); XSync(d, False);
    sample(PX, PY, 400);
    unsigned to_a = 0;
    while (XCheckMaskEvent(d, ButtonPressMask, &e)) to_a += e.xbutton.window == a;
    printf("METRIC: escape keyboard free after %ldms, then shown=%06lx bar=%06lx click_to_client=%u\n",
        freed, shown, bar_shown, to_a);
    report("escape", ov, RED);
    check(freed >= 0 && shown != RED,
        "escape: keyboard free at once while the overview image is still on screen");
    check(to_a == 1, "escape: a click during the fade-out reaches the client below");
    check(bar_shown != BAR, "escape: the fading image is drawn above the bar");
    check(fade_ok(ov, RED) || (blends(ov, RED) && final() == RED && settle(RED) <= FADE_MS + 120),
        "escape: fade-out, then the exact final frame");
    Window focused; int revert;
    XGetInputFocus(d, &focused, &revert);
    check(cardinal(A_DESKTOP) == 0 && focused == a, "escape: the same desktop and focus as before");

    /* 3. Commit with real keys: desktop and focus change at once; the
     *    overview image fades out over the new workspace. */
    open_overview(500);
    tap(XK_Right);
    sleep_ms(50);
    tap(XK_Return);
    long long t0 = now_ms();
    focused = None;
    long desk = -1;
    while (now_ms() - t0 < 100) {
        desk = cardinal(A_DESKTOP);
        XGetInputFocus(d, &focused, &revert);
        if (desk == 1 && focused == b) break;
        sleep_ms(2);
    }
    long took = (long)(now_ms() - t0);
    unsigned long at_commit = pixel(PX, PY);
    sample(PX, PY, 500);
    printf("METRIC: commit desktop after %ldms, shown then=%06lx focus=0x%lx b=0x%lx\n",
        took, at_commit, (unsigned long)focused, (unsigned long)b);
    report("commit", ov, BLUE);
    check(desk == 1 && focused == b && at_commit != BLUE,
        "commit: desktop and focus change at once, while the overview image is still on screen");
    check(fade_ok(ov, BLUE), "commit: the overview fades out over the new workspace");
    desktop_wait(0);
    sleep_ms(300);

    /* 3b. 4B: the selection highlight moves through between colours. The
     *     selection stays pending: the desktop does not change. */
    open_overview(500);
    unsigned changed, between;
    highlight_move(&changed, &between);
    printf("METRIC: highlight move changed=%u between=%u\n", changed, between);
    check(changed > 0 && between > 0 && cardinal(A_DESKTOP) == 0,
        "highlight: the selection moves through between colours, still pending");
    tap(XK_Escape);
    sleep_ms(500);

    /* 4. Click during the fade-in: measured, not judged. */
    key(XK_o, Mod4Mask);
    sleep_ms(20);
    shown = pixel(PX, PY);
    fake_motion(d, -1, 512, 384, 0);
    fake_button(d, 1, True, 0); fake_button(d, 1, False, 0); XSync(d, False);
    sleep_ms(400);
    printf("METRIC: click at 20 ms into the fade-in shown=%06lx desktop_after=%ld\n",
        shown, cardinal(A_DESKTOP));
    if (pixel(PX, PY) == ov) { tap(XK_Escape); sleep_ms(400); }
    fake_motion(d, -1, 512, PY, 0); XSync(d, False);
    desktop_wait(0);
    sleep_ms(300);

    /* 5. A plain override-redirect window stays instant. */
    XSetWindowAttributes wa = {0};
    wa.override_redirect = True; wa.background_pixel = BLUE;
    Window menu = XCreateWindow(d, root, 100, 300, 200, 200, 0, CopyFromParent, InputOutput,
        CopyFromParent, CWOverrideRedirect | CWBackPixel, &wa);
    XMapWindow(d, menu); XFlush(d);
    sample(200, 400, 200);
    report("menu map", RED, BLUE);
    int menu_in = instant_ok(RED, BLUE);
    XUnmapWindow(d, menu); XFlush(d);
    sample(200, 400, 200);
    check(menu_in && instant_ok(BLUE, RED), "plain override-redirect window: instant map and unmap");
    XDestroyWindow(d, menu);

    /* 5b. A menu mapped while the overview is open stays instant: only the
     *     published overview window fades. */
    open_overview(500);
    menu = XCreateWindow(d, root, 100, 300, 200, 200, 0, CopyFromParent, InputOutput,
        CopyFromParent, CWOverrideRedirect | CWBackPixel, &wa);
    XMapWindow(d, menu); XFlush(d);
    sample(200, 400, 200);
    report("menu over overview", ov, BLUE);
    check(instant_ok(ov, BLUE), "menu mapped while the overview is open: instant");
    XDestroyWindow(d, menu); XFlush(d);
    tap(XK_Escape);
    sleep_ms(500);

    /* 5c. Open fails because another client holds the keyboard: no overview
     *     image stays, and nothing leaks. */
    assert(XGrabKeyboard(d, root, False, GrabModeAsync, GrabModeAsync, CurrentTime) == GrabSuccess);
    key(XK_o, Mod4Mask);
    sample(PX, PY, 400);
    unsigned ov_seen = 0;
    for (unsigned i = 0; i < trace.n; i++) ov_seen += trace.p[i] == ov;
    XUngrabKeyboard(d, CurrentTime); XSync(d, False);
    report("grab failure", ov, RED);
    printf("METRIC: grab failure overview frames=%u/%u\n", ov_seen, trace.n);
    check(final() == RED && !ov_seen && settle(RED) <= 120 && keyboard_free(),
        "open with the keyboard held elsewhere: no overview image stays, keyboard free");

    /* 6. Toggle storm: no stacked grab or image. */
    for (int i = 0; i < 10; i++) { key(XK_o, Mod4Mask); sleep_ms(30); }
    sample(PX, PY, 500);
    report("storm", ov, RED);
    check(final() == RED && settle(RED) <= FADE_MS + 120 && keyboard_free(),
        "toggle storm: exact final frame, keyboard free");

    /* 7. Instant cases. */
    reload(0, 0, 1);
    open_overview(300);
    check(instant_ok(RED, ov), "animation_overview_ms = 0: instant open");
    highlight_move(&changed, &between);
    printf("METRIC: highlight move (off) changed=%u between=%u\n", changed, between);
    check(changed > 0 && between == 0, "animation_overview_ms = 0: instant highlight");
    tap(XK_Escape); sample(PX, PY, 300);
    check(instant_ok(ov, RED), "animation_overview_ms = 0: instant close");
    reload(FADE_MS, 1, 1);
    open_overview(300);
    check(instant_ok(RED, ov), "reduce_motion = 1: instant open");
    tap(XK_Escape); sample(PX, PY, 300);
    check(instant_ok(ov, RED), "reduce_motion = 1: instant close");
    reload(FADE_MS, 0, 0);
    open_overview(300);
    check(instant_ok(RED, ov), "animations = 0: instant open");
    tap(XK_Escape); sample(PX, PY, 300);
    check(instant_ok(ov, RED), "animations = 0: instant close");
    reload(FADE_MS, 0, 1);

    /* 8. Resources return to the baseline. */
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
