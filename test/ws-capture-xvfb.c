/* Phase 3-P1 workspace capture prototype on a private Xvfb. Test-only.
 * "model" mode: compositor model process (automatic redirection, named pixmaps).
 * default mode: simulated WM + app connections that drive adverse schedules. */
#define _POSIX_C_SOURCE 200809L
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/Xlib-xcb.h>
#include <xcb/xcb.h>
#include <xcb/res.h>
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "ws-capture-prototype.h"

static int64_t mono_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static int64_t mono_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}
static void sleep_ms(long ms) {
    struct timespec t = {ms / 1000, (ms % 1000) * 1000000L};
    while (nanosleep(&t, &t) && errno == EINTR) {}
}
static unsigned long prop_word(Display *d, Window w, Atom atom, Atom type) {
    Atom actual; int format; unsigned long n = 0, extra = 0, value = 0;
    unsigned char *data = NULL;
    if (XGetWindowProperty(d, w, atom, 0, 1, False, type, &actual, &format, &n, &extra,
        &data) == Success && actual == type && format == 32 && n == 1 && data)
        value = *(unsigned long *)data;
    if (data) XFree(data);
    return value;
}

/* ------------------------------------------------------------------ model */
typedef struct {
    Window id;
    int mapped;
    uint64_t generation;
    Pixmap pixmap;
    unsigned w, h, depth;
} Track;
static Display *md;
static Window mroot, mowner;
static Track tracks[64];
static WSScene scene;
static uint32_t latest_sequence;
static unsigned bpp_of[33], pad_of[33];
static int trapping, trapped;
static unsigned unexpected_errors;
static volatile sig_atomic_t model_quit;
static Atom A_OWNER, A_EPOCH, A_TXN, A_EVENT, A_DESKTOP, A_COMP;
static int scene_xerror;

static int model_error(Display *d, XErrorEvent *e) {
    if (trapping) { trapped = 1; return 0; }
    /* A kept image died under us: drop it from the scene, end it at loop top. */
    for (uint32_t i = 0; scene.phase != WSP_IDLE && i < scene.txn.count; i++)
        if (scene.m[i].pixmap && e->resourceid == scene.m[i].pixmap) {
            scene.m[i].pixmap = 0; scene_xerror = 1; return 0;
        }
    char text[128];
    XGetErrorText(d, e->error_code, text, sizeof(text));
    fprintf(stderr, "model: unexpected X error %s req=%u/%u\n", text, e->request_code, e->minor_code);
    unexpected_errors++;
    return 0;
}
static Track *track(Window id, int create) {
    Track *free_slot = NULL;
    for (unsigned i = 0; i < 64; i++) {
        if (tracks[i].id == id) return &tracks[i];
        if (!tracks[i].id && !free_slot) free_slot = &tracks[i];
    }
    if (!create || !free_slot) return NULL;
    *free_slot = (Track){0};
    free_slot->id = id;
    return free_slot;
}
static void release_pixmap(unsigned long pixmap) { XFreePixmap(md, pixmap); }
static void drop_pixmap(Track *t) {
    if (t->pixmap) XFreePixmap(md, t->pixmap);
    t->pixmap = None; t->w = t->h = t->depth = 0;
}
/* Name the window storage. It fails (BadMatch) once the server has unmapped it. */
static void name_pixmap(Track *t) {
    drop_pixmap(t);
    trapping = 1; trapped = 0;
    Pixmap pm = XCompositeNameWindowPixmap(md, t->id);
    Window r; int x, y; unsigned w = 0, h = 0, b, depth = 0;
    Status ok = XGetGeometry(md, pm, &r, &x, &y, &w, &h, &b, &depth);
    XSync(md, False);
    trapping = 0;
    if (trapped || !ok) {
        if (!trapped) XFreePixmap(md, pm);
        return;
    }
    t->pixmap = pm; t->w = w; t->h = h; t->depth = depth;
}
static void held(void) {
    printf("HELD %" PRIu32, scene.txn.sequence);
    for (uint32_t i = 0; i < scene.txn.count; i++) printf(" 0x%lx", scene.m[i].pixmap);
    printf("\n");
    fflush(stdout);
}
static void report(uint32_t sequence, int reason) {
    printf("SCENE %" PRIu32 " BYPASS %s\n", sequence, wsp_reason_names[reason]);
    fflush(stdout);
}
static void scene_end(int reason) {
    if (scene.phase == WSP_IDLE) return;
    report(scene.txn.sequence, reason);
    wsp_clear(&scene, release_pixmap);
}
static unsigned long center_pixel(Pixmap pm, unsigned w, unsigned h) {
    XImage *image = XGetImage(md, pm, (int)w / 2, (int)h / 2, 1, 1, AllPlanes, ZPixmap);
    if (!image) return ~0UL;
    unsigned long p = XGetPixel(image, 0, 0) & 0xffffff;
    XDestroyImage(image);
    return p;
}
static void scene_end(int reason);
static void admit(void) {
    unsigned w[WSP_MEMBERS] = {0}, h[WSP_MEMBERS] = {0}, order[WSP_MEMBERS];
    /* Validate every kept image before use; a dead one ends the scene. */
    for (uint32_t i = 0; i < scene.txn.count; i++) {
        Window r; int x, y; unsigned b, depth;
        trapping = 1; trapped = 0;
        Status ok = scene.m[i].pixmap &&
            XGetGeometry(md, scene.m[i].pixmap, &r, &x, &y, &w[i], &h[i], &b, &depth);
        XSync(md, False);
        trapping = 0;
        if (!ok || trapped) scene.m[i].pixmap = 0;
    }
    for (uint32_t i = 0; i < scene.txn.count; i++)
        if (!scene.m[i].pixmap) { scene_end(WSP_XERROR); return; }
    printf("SCENE %" PRIu32 " ADMIT %" PRIu32, scene.txn.sequence, scene.txn.count);
    wsp_stack_order(&scene, order);
    for (uint32_t i = 0; i < scene.txn.count; i++)
        printf(" %06lx", center_pixel(scene.m[order[i]].pixmap, w[order[i]], h[order[i]]));
    printf("\n");
    fflush(stdout);
    wsp_clear(&scene, release_pixmap);
}
static void prepare(const XClientMessageEvent *e) {
    uint32_t sequence = (uint32_t)e->data.l[2];
    Window owner = (Window)prop_word(md, mroot, A_OWNER, XA_WINDOW);
    if (!owner || (Window)e->data.l[1] != owner || !wsp_newer(sequence, latest_sequence)) {
        report(sequence, WSP_STALE); return;
    }
    latest_sequence = sequence;
    if (e->data.l[3] == WSP_CANCEL) { scene_end(WSP_CANCELLED); return; }
    /* Newest wins: an older scene never survives a newer PREPARE. */
    scene_end(WSP_SUPERSEDED);
    uint32_t epoch = (uint32_t)prop_word(md, mroot, A_EPOCH, XA_CARDINAL);
    Atom type; int format; unsigned long n = 0, extra = 0;
    unsigned char *data = NULL;
    WSTxn t;
    int ok = XGetWindowProperty(md, mroot, A_TXN, 0, WSP_WORDS, False, XA_CARDINAL, &type,
        &format, &n, &extra, &data) == Success && type == XA_CARDINAL && format == 32 &&
        wsp_decode(&t, (unsigned long *)data, n, extra, (uint32_t)owner, (uint32_t)mowner, epoch);
    if (data) XFree(data);
    if (!ok) { report(sequence, WSP_INVALID); return; }
    /* The property may already hold a newer switch: never mix the two. */
    if (t.sequence != sequence) {
        report(sequence, wsp_newer(t.sequence, sequence) ? WSP_OVERWRITTEN : WSP_INVALID);
        return;
    }
    if (!wsp_age_ok(t.sent_ms, (uint32_t)mono_ms(), t.deadline_ms)) { report(sequence, WSP_LATE); return; }
    uint64_t bytes[WSP_MEMBERS] = {0}, generations[WSP_MEMBERS] = {0};
    for (uint32_t i = 0; i < t.count; i++) {
        Track *k = track(t.members[i], 0);
        if (!k || !k->mapped || !k->pixmap || k->depth > 32 ||
            !wsp_image_bytes(&bytes[i], k->w, k->h, bpp_of[k->depth], pad_of[k->depth])) {
            report(sequence, WSP_UNNAMED); return;
        }
        generations[i] = k->generation;
    }
    if (!wsp_begin(&scene, &t, bytes, generations, mono_ms())) {
        report(sequence, WSP_BUDGET_EXCEEDED); return;
    }
    /* Unmapped windows keep their place, so the current tree gives the order. */
    Window r, p, *kids = NULL; unsigned nk = 0, found = 0;
    if (XQueryTree(md, mroot, &r, &p, &kids, &nk))
        for (unsigned k = 0; k < nk; k++) {
            int index = wsp_member(&scene, (uint32_t)kids[k]);
            if (index >= 0) { scene.m[index].rank = k; found++; }
        }
    if (kids) XFree(kids);
    if (found != t.count) scene_end(WSP_UNNAMED);
}
static void model_event(XEvent *ev) {
    switch (ev->type) {
    case MapNotify: {
        if (ev->xmap.event != mroot || ev->xmap.override_redirect) break;
        Track *t = track(ev->xmap.window, 1);
        if (!t) break;
        t->mapped = 1; t->generation++;
        name_pixmap(t);
        break;
    }
    case ConfigureNotify: {
        if (ev->xconfigure.event != mroot) break;
        Track *t = track(ev->xconfigure.window, 0);
        /* New storage on resize: the old named pixmap is no longer fresh. */
        if (t && t->pixmap && ((unsigned)ev->xconfigure.width != t->w ||
            (unsigned)ev->xconfigure.height != t->h)) {
            if (t->mapped) name_pixmap(t); else drop_pixmap(t);
        }
        break;
    }
    case UnmapNotify: {
        if (ev->xunmap.event != mroot || ev->xunmap.send_event) break;
        Track *t = track(ev->xunmap.window, 0);
        if (!t) break;
        int index = wsp_member(&scene, (uint32_t)t->id);
        if (index >= 0 && scene.phase == WSP_COLLECTING) {
            if (wsp_retain(&scene, index, t->pixmap, t->generation)) {
                t->pixmap = None;
                if (scene.phase == WSP_READY && scene.committed) admit();
                else if (scene.phase == WSP_READY) held();
            } else scene_end(WSP_UNNAMED);
        }
        t->mapped = 0;
        drop_pixmap(t);
        break;
    }
    case DestroyNotify: {
        if (ev->xdestroywindow.event != mroot) break;
        Track *t = track(ev->xdestroywindow.window, 0);
        if (t) { drop_pixmap(t); *t = (Track){0}; }
        break;
    }
    case PropertyNotify:
        if (ev->xproperty.window != mroot) break;
        if (ev->xproperty.atom == A_OWNER) scene_end(WSP_OWNER);
        else if (ev->xproperty.atom == A_COMP &&
                 (Window)prop_word(md, mroot, A_COMP, XA_WINDOW) != mowner) scene_end(WSP_REPLACED);
        else if (ev->xproperty.atom == A_EPOCH) scene_end(WSP_EPOCH);
        else if (ev->xproperty.atom == A_DESKTOP && scene.phase != WSP_IDLE &&
                 prop_word(md, mroot, A_DESKTOP, XA_CARDINAL) == scene.txn.to) {
            /* daniwm publishes the desktop before it unmaps the members. */
            if (scene.phase == WSP_READY) admit(); else scene.committed = 1;
        }
        break;
    case ClientMessage:
        if (ev->xclient.window == mroot && ev->xclient.message_type == A_EVENT &&
            ev->xclient.format == 32 && ev->xclient.data.l[0] == WSP_VERSION)
            prepare(&ev->xclient);
        break;
    }
}
static void on_term(int sig) { (void)sig; model_quit = 1; }
static int model_main(const char *display) {
    md = XOpenDisplay(display);
    if (!md) return 2;
    XSetErrorHandler(model_error);
    signal(SIGTERM, on_term);
    mroot = DefaultRootWindow(md);
    A_OWNER = XInternAtom(md, WSP_OWNER_ATOM, False);
    A_EPOCH = XInternAtom(md, WSP_EPOCH_ATOM, False);
    A_TXN = XInternAtom(md, WSP_TXN_ATOM, False);
    A_EVENT = XInternAtom(md, WSP_EVENT_ATOM, False);
    A_DESKTOP = XInternAtom(md, "_NET_CURRENT_DESKTOP", False);
    A_COMP = XInternAtom(md, WSP_COMP_ATOM, False);
    int nf;
    XPixmapFormatValues *f = XListPixmapFormats(md, &nf);
    for (int i = 0; f && i < nf; i++) if (f[i].depth > 0 && f[i].depth <= 32) {
        bpp_of[f[i].depth] = (unsigned)f[i].bits_per_pixel;
        pad_of[f[i].depth] = (unsigned)f[i].scanline_pad;
    }
    if (f) XFree(f);
    XGrabServer(md); /* consistent startup snapshot only; never during a switch */
    XCompositeRedirectSubwindows(md, mroot, CompositeRedirectAutomatic);
    XSelectInput(md, mroot, SubstructureNotifyMask | PropertyChangeMask);
    Window r, p, *kids = NULL; unsigned nk = 0;
    XQueryTree(md, mroot, &r, &p, &kids, &nk);
    for (unsigned i = 0; i < nk; i++) {
        XWindowAttributes a;
        if (!XGetWindowAttributes(md, kids[i], &a) || a.override_redirect ||
            a.class != InputOutput || a.map_state != IsViewable) continue;
        Track *t = track(kids[i], 1);
        if (t) { t->mapped = 1; t->generation = 1; name_pixmap(t); }
    }
    if (kids) XFree(kids);
    XUngrabServer(md);
    XSetWindowAttributes sa = {0}; sa.override_redirect = True;
    mowner = XCreateWindow(md, mroot, -10, -10, 1, 1, 0, CopyFromParent, InputOnly,
        CopyFromParent, CWOverrideRedirect, &sa);
    unsigned long version = WSP_VERSION;
    XChangeProperty(md, mowner, XInternAtom(md, WSP_CAP_ATOM, False), XA_CARDINAL, 32,
        PropModeReplace, (unsigned char *)&version, 1);
    XChangeProperty(md, mroot, XInternAtom(md, WSP_COMP_ATOM, False), XA_WINDOW, 32,
        PropModeReplace, (unsigned char *)&mowner, 1);
    XSync(md, False);
    printf("READY 0x%lx\n", mowner);
    fflush(stdout);
    int fd = ConnectionNumber(md);
    while (!model_quit) {
        while (XPending(md)) {
            XEvent ev; XNextEvent(md, &ev); model_event(&ev);
            if (scene_xerror) { scene_xerror = 0; scene_end(WSP_XERROR); }
        }
        if (wsp_expired(&scene, mono_ms())) scene_end(WSP_EXPIRED);
        fd_set set; FD_ZERO(&set); FD_SET(fd, &set);
        struct timeval tv = {0, 20000};
        select(fd + 1, &set, NULL, NULL, &tv);
    }
    scene_end(WSP_CANCELLED);
    for (unsigned i = 0; i < 64; i++) if (tracks[i].id) drop_pixmap(&tracks[i]);
    XSync(md, False);
    printf("ERRORS %u\n", unexpected_errors);
    fflush(stdout);
    XCloseDisplay(md);
    return 0;
}

/* ----------------------------------------------------------------- driver */
static Display *wm, *app;
static Window root, wm_owner;
static Window ws[2][8];
static int hidden[2][8]; /* like monocle-hidden clients: never mapped */
static unsigned nws[2], current;
static int list_all; /* negative control: also list hidden clients */
static uint32_t sequence, epoch = 1;
static pid_t model_pid;
static FILE *model_out;
static int failures;
static Atom D_OWNER, D_EPOCH, D_TXN, D_EVENT, D_DESKTOP, D_COMP, D_CAP;
static int64_t last_enqueue_us, last_complete_us;

static void check(int ok, const char *what) {
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    fflush(stdout);
    if (!ok) failures++;
}
static void set_word(Display *d, Atom atom, Atom type, unsigned long value) {
    XChangeProperty(d, root, atom, type, 32, PropModeReplace, (unsigned char *)&value, 1);
}
static int capability(void) {
    Window comp = (Window)prop_word(wm, root, D_COMP, XA_WINDOW);
    if (!comp) return 0;
    return prop_word(wm, comp, D_CAP, XA_CARDINAL) == WSP_VERSION;
}
static Window client(unsigned slot, unsigned long color, unsigned w, unsigned h) {
    Window win = XCreateSimpleWindow(app, root, 20 + 30 * (int)nws[slot], 20, w, h, 0, 0, color);
    XSync(app, False);
    hidden[slot][nws[slot]] = 0;
    ws[slot][nws[slot]++] = win;
    if (slot == current) { XMapWindow(wm, win); XSync(wm, False); }
    return win;
}
static void forget(unsigned slot, Window win) {
    for (unsigned i = 0; i < nws[slot]; i++) if (ws[slot][i] == win) {
        nws[slot]--;
        ws[slot][i] = ws[slot][nws[slot]]; hidden[slot][i] = hidden[slot][nws[slot]];
        return;
    }
}
static void paint(Window win, unsigned long color) {
    GC gc = XCreateGC(app, win, 0, NULL);
    XSetForeground(app, gc, color);
    XFillRectangle(app, win, gc, 0, 0, 4096, 4096);
    XFreeGC(app, gc);
    XSync(app, False);
}
enum { HOOK_NONE, HOOK_OWNER, HOOK_EPOCH, HOOK_DESTROY, HOOK_NO_COMMIT, HOOK_COMP, HOOK_FREE };
static Window destroy_target;
static long hook_delay_ms;
/* 0: daniwm view() order (desktop before unmaps). 1: commit after the maps. */
static int commit_last;
static void change(int hook) {
    if (hook != HOOK_OWNER && hook != HOOK_EPOCH && hook != HOOK_COMP) return;
    /* Test ordering control only: let the model reach the in-flight scene. */
    if (hook_delay_ms) { XSync(wm, False); sleep_ms(hook_delay_ms); }
    if (hook == HOOK_OWNER) {
        wm_owner = XCreateSimpleWindow(wm, root, -5, -5, 1, 1, 0, 0, 0);
        set_word(wm, D_OWNER, XA_WINDOW, wm_owner);
    } else if (hook == HOOK_EPOCH) set_word(wm, D_EPOCH, XA_CARDINAL, ++epoch);
    else set_word(wm, D_COMP, XA_WINDOW, wm_owner); /* another compositor owner */
}
static int model_line(char *line, size_t size, int timeout_ms);
/* Test ordering control: wait until the model holds every image. */
static Pixmap wait_held(uint32_t seq) {
    char line[256], want[32];
    snprintf(want, sizeof(want), "HELD %" PRIu32 " ", seq);
    XSync(wm, False);
    while (model_line(line, sizeof(line), 2500))
        if (!strncmp(line, want, strlen(want)))
            return (Pixmap)strtoul(line + strlen(want), NULL, 16);
    return None;
}
/* The simulated WM never reads from or waits for the compositor model. */
static uint32_t switch_ws(unsigned to, uint32_t deadline, int hook) {
    unsigned from = current;
    int64_t t0 = mono_us();
    uint32_t seq = 0;
    if (capability()) {
        WSTxn t = {0};
        Window comp = (Window)prop_word(wm, root, D_COMP, XA_WINDOW);
        seq = ++sequence;
        t.wm = (uint32_t)wm_owner; t.compositor = (uint32_t)comp; t.epoch = epoch;
        t.sequence = seq; t.from = from; t.to = to; t.sent_ms = (uint32_t)mono_ms();
        t.deadline_ms = deadline;
        /* Members are the viewable outgoing clients only. */
        for (unsigned i = 0; i < nws[from]; i++)
            if (list_all || !hidden[from][i]) t.members[t.count++] = (uint32_t)ws[from][i];
        unsigned long words[WSP_WORDS];
        size_t n = wsp_encode(words, &t);
        XChangeProperty(wm, root, D_TXN, XA_CARDINAL, 32, PropModeReplace,
            (unsigned char *)words, (int)n);
        XEvent e = {0};
        e.xclient.type = ClientMessage; e.xclient.window = root;
        e.xclient.message_type = D_EVENT; e.xclient.format = 32;
        e.xclient.data.l[0] = WSP_VERSION; e.xclient.data.l[1] = (long)wm_owner;
        e.xclient.data.l[2] = (long)seq; e.xclient.data.l[3] = WSP_PREPARE;
        XSendEvent(wm, root, False, SubstructureNotifyMask, &e);
    }
    if (!commit_last && hook != HOOK_NO_COMMIT) set_word(wm, D_DESKTOP, XA_CARDINAL, to);
    /* daniwm order: owner/epoch change lands while the scene collects. */
    if (!commit_last) change(hook);
    for (unsigned i = 0; i < nws[from]; i++) if (!hidden[from][i]) XUnmapWindow(wm, ws[from][i]);
    for (unsigned i = 0; i < nws[to]; i++) if (!hidden[to][i]) XMapWindow(wm, ws[to][i]);
    /* Test-only fault: free one kept image from another connection. */
    if (hook == HOOK_FREE) {
        Pixmap victim = wait_held(seq);
        if (victim) { XFreePixmap(wm, victim); XSync(wm, False); }
    }
    if (hook == HOOK_DESTROY) {
        wait_held(seq);
        XDestroyWindow(app, destroy_target); XSync(app, False);
    }
    /* Commit-last order: the change lands while the scene holds every image. */
    if (commit_last) change(hook);
    if (commit_last && hook != HOOK_NO_COMMIT) set_word(wm, D_DESKTOP, XA_CARDINAL, to);
    XFlush(wm);
    last_enqueue_us = mono_us() - t0;
    XSync(wm, False);
    last_complete_us = mono_us() - t0;
    current = to;
    return seq;
}
/* Read one model line with a deadline. */
static int model_line(char *line, size_t size, int timeout_ms) {
    struct pollfd p = {fileno(model_out), POLLIN, 0};
    int64_t end = mono_ms() + timeout_ms;
    for (;;) {
        int left = (int)(end - mono_ms());
        if (left < 0) left = 0;
        if (poll(&p, 1, left) <= 0) return 0;
        if (!fgets(line, (int)size, model_out)) return 0;
        line[strcspn(line, "\n")] = 0;
        return 1;
    }
}
static int scene_line(uint32_t seq, char *out, size_t size) {
    char line[256], want[32];
    snprintf(want, sizeof(want), "SCENE %" PRIu32 " ", seq);
    while (model_line(line, sizeof(line), 2500)) {
        if (!strncmp(line, want, strlen(want))) {
            snprintf(out, size, "%s", line + strlen(want));
            return 1;
        }
        if (strncmp(line, "HELD ", 5)) printf("INFO: model %s\n", line);
    }
    snprintf(out, size, "(no report)");
    return 0;
}
static int expect(uint32_t seq, const char *result, const char *what) {
    char got[256], label[640];
    scene_line(seq, got, sizeof(got));
    int ok = !strncmp(got, result, strlen(result));
    snprintf(label, sizeof(label), "%s [seq %" PRIu32 ": %s]", what, seq, got);
    check(ok, label);
    return ok;
}
static void settle(unsigned to) {
    uint32_t s = switch_ws(to, 1000, HOOK_NONE);
    char got[256];
    if (s) scene_line(s, got, sizeof(got));
    sleep_ms(30);
}
static void model_stop(void) { kill(model_pid, SIGSTOP); }
static void model_cont(void) { kill(model_pid, SIGCONT); }
static void resources(Window anchor, uint64_t *count, uint64_t *bytes) {
    xcb_connection_t *c = XGetXCBConnection(wm);
    XSync(wm, False);
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
int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "model")) return model_main(argv[2]);
    assert(argc == 3 && argv[2][0] == ':');
    const char *display = argv[2];
    wm = XOpenDisplay(display); app = XOpenDisplay(display);
    assert(wm && app);
    root = DefaultRootWindow(wm);
    D_OWNER = XInternAtom(wm, WSP_OWNER_ATOM, False);
    D_EPOCH = XInternAtom(wm, WSP_EPOCH_ATOM, False);
    D_TXN = XInternAtom(wm, WSP_TXN_ATOM, False);
    D_EVENT = XInternAtom(wm, WSP_EVENT_ATOM, False);
    D_DESKTOP = XInternAtom(wm, "_NET_CURRENT_DESKTOP", False);
    D_COMP = XInternAtom(wm, WSP_COMP_ATOM, False);
    D_CAP = XInternAtom(wm, WSP_CAP_ATOM, False);
    wm_owner = XCreateSimpleWindow(wm, root, -5, -5, 1, 1, 0, 0, 0);
    set_word(wm, D_OWNER, XA_WINDOW, wm_owner);
    set_word(wm, D_EPOCH, XA_CARDINAL, epoch);
    set_word(wm, D_DESKTOP, XA_CARDINAL, 0);
    XSync(wm, False);

    Window w1 = client(0, 0xcc0000, 120, 90);
    Window w2 = client(0, 0x00cc00, 120, 90);
    Window w3 = client(1, 0x0000cc, 120, 90);
    (void)w3;

    /* 0. No capability: the WM publishes nothing and stays instant. */
    switch_ws(1, 1000, HOOK_NONE); switch_ws(0, 1000, HOOK_NONE);
    Atom type; int format; unsigned long n = 0, extra = 0; unsigned char *data = NULL;
    XGetWindowProperty(wm, root, D_TXN, 0, 1, False, AnyPropertyType, &type, &format, &n, &extra, &data);
    if (data) XFree(data);
    check(type == None && sequence == 0, "no capability: WM sends no transaction");

    /* Start the model as a separate process. */
    int fds[2];
    assert(pipe(fds) == 0);
    model_pid = fork();
    assert(model_pid >= 0);
    if (!model_pid) {
        dup2(fds[1], STDOUT_FILENO); close(fds[0]); close(fds[1]);
        close(ConnectionNumber(wm)); close(ConnectionNumber(app));
        execl("/proc/self/exe", argv[0], "model", display, (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    model_out = fdopen(fds[0], "r");
    setvbuf(model_out, NULL, _IONBF, 0); /* poll() must see every pending line */
    char line[256];
    assert(model_line(line, sizeof(line), 3000) && !strncmp(line, "READY", 5));
    Window model_owner = (Window)strtoul(line + 6, NULL, 16);
    sleep_ms(50);

    /* 1. Positive control (deliberately ordered): new content, app XSync, idle model. */
    paint(w1, 0xffff00);
    sleep_ms(50);
    uint32_t s = switch_ws(1, 1000, HOOK_NONE);
    expect(s, "ADMIT 2 ffff00 00cc00", "positive control (daniwm order): fresh, complete outgoing scene");
    settle(0);
    sleep_ms(50);
    commit_last = 1;
    s = switch_ws(1, 1000, HOOK_NONE);
    commit_last = 0;
    expect(s, "ADMIT", "commit-last order: complete scene is admitted at commit");
    settle(0);

    /* 2. Model behind: a window maps and leaves before the model names it. */
    model_stop();
    Window w4 = client(0, 0xcc00cc, 120, 90);
    s = switch_ws(1, 1000, HOOK_NONE);
    model_cont();
    expect(s, "BYPASS unnamed", "model behind: unnamed member bypasses the whole scene");
    settle(0);

    /* 3. Resize just before the switch while the model is behind. */
    sleep_ms(50);
    model_stop();
    XResizeWindow(wm, w1, 140, 100); XSync(wm, False);
    s = switch_ws(1, 1000, HOOK_NONE);
    model_cont();
    expect(s, "BYPASS unnamed", "resize then switch: stale-size image is never admitted");
    settle(0);

    /* 4. Overwritten property: two switches before the model reads the first. */
    sleep_ms(50);
    model_stop();
    uint32_t first = switch_ws(1, 1000, HOOK_NONE);
    uint32_t second = switch_ws(0, 1000, HOOK_NONE);
    model_cont();
    expect(first, "BYPASS overwritten", "overwritten property: older switch bypasses");
    char got[256];
    scene_line(second, got, sizeof(got));
    printf("METRIC: overwritten follow-up seq %" PRIu32 ": %s\n", second, got);
    check(strcmp(got, "(no report)") != 0, "follow-up switch is reported");

    /* 5. No-wait proof: the WM completes while the model is stopped; late PREPARE bypasses. */
    sleep_ms(50);
    model_stop();
    s = switch_ws(1, 100, HOOK_NONE);
    int64_t enqueue = last_enqueue_us, complete = last_complete_us;
    sleep_ms(300);
    model_cont();
    printf("METRIC: WM switch with stopped model: enqueue=%" PRId64 " us complete=%" PRId64 " us\n",
           enqueue, complete);
    check(complete < 50000, "WM switch completes with the model stopped (< 50 ms)");
    expect(s, "BYPASS late", "late PREPARE after its deadline bypasses");
    settle(0);

    /* 6. Owner change and 7. monitor epoch bump between PREPARE and commit.
     * Ordered variant (labeled): the model holds the scene when the change
     * lands. Race variant: the change lands before the model reads PREPARE. */
    hook_delay_ms = 100;
    sleep_ms(50);
    s = switch_ws(1, 1000, HOOK_OWNER);
    expect(s, "BYPASS owner", "ordered: owner change cancels the in-flight scene");
    settle(0);
    sleep_ms(50);
    s = switch_ws(1, 1000, HOOK_EPOCH);
    expect(s, "BYPASS epoch", "ordered: monitor epoch change cancels the in-flight scene");
    settle(0);
    hook_delay_ms = 0;
    sleep_ms(50);
    model_stop();
    s = switch_ws(1, 1000, HOOK_OWNER);
    model_cont();
    expect(s, "BYPASS", "race: owner change before PREPARE is read never admits");
    settle(0);
    sleep_ms(50);
    model_stop();
    s = switch_ws(1, 1000, HOOK_EPOCH);
    model_cont();
    expect(s, "BYPASS", "race: epoch change before PREPARE is read never admits");
    settle(0);

    /* 8. TTL: no commit arrives; the scene expires and is freed. */
    sleep_ms(50);
    s = switch_ws(1, 1000, HOOK_NO_COMMIT);
    expect(s, "BYPASS expired", "missing commit expires after the TTL");
    set_word(wm, D_DESKTOP, XA_CARDINAL, 1); XSync(wm, False);
    settle(0);

    /* 9. Destroy during retention: the kept image stays valid. */
    sleep_ms(50);
    /* Remapped windows show their background again (cc0000 for w1). */
    paint(w2, 0x00ffff);
    sleep_ms(50);
    destroy_target = w2;
    commit_last = 1; /* ordered: hold the scene across the destroy */
    s = switch_ws(1, 1000, HOOK_DESTROY);
    commit_last = 0;
    char want[64];
    snprintf(want, sizeof(want), "ADMIT %u cc0000 00ffff cc00cc", nws[0]);
    expect(s, want, "ordered (commit last): destroy during retention, kept image stays valid");
    forget(0, w2);
    settle(0);

    /* 10. Over budget: admission fails before any retention. */
    sleep_ms(50);
    Window big = client(0, 0x808080, 4096, 2048);
    sleep_ms(100);
    s = switch_ws(1, 1000, HOOK_NONE);
    expect(s, "BYPASS budget", "over-budget scene bypasses before retention");
    settle(0);
    XDestroyWindow(app, big); XSync(app, False);
    forget(0, big);
    sleep_ms(50);

    /* 11. Rapid A->B->A with a running model: the older switch never revives. */
    first = switch_ws(1, 1000, HOOK_NONE);
    second = switch_ws(0, 1000, HOOK_NONE);
    char r1[256], r2[256];
    scene_line(first, r1, sizeof(r1));
    scene_line(second, r2, sizeof(r2));
    printf("METRIC: rapid switch outcomes: %s / %s\n", r1, r2);
    check((!strncmp(r1, "ADMIT", 5) || !strcmp(r1, "BYPASS superseded") ||
           !strcmp(r1, "BYPASS unnamed")) && strcmp(r2, "(no report)"),
          "rapid A->B->A: each switch reported once, newest wins");
    sleep_ms(100);

    /* 12a. Member set: only viewable clients. A monocle-like hidden client stays out. */
    Window shy = client(0, 0x404040, 120, 90);
    XUnmapWindow(wm, shy); XSync(wm, False);
    hidden[0][nws[0] - 1] = 1;
    sleep_ms(50);
    unsigned viewable = 0;
    for (unsigned i = 0; i < nws[0]; i++) if (!hidden[0][i]) viewable++;
    s = switch_ws(1, 1000, HOOK_NONE);
    snprintf(want, sizeof(want), "ADMIT %u", viewable);
    expect(s, want, "viewable members only: a hidden client does not block the scene");
    settle(0);
    sleep_ms(50);
    list_all = 1;
    s = switch_ws(1, 1000, HOOK_NONE);
    list_all = 0;
    expect(s, "BYPASS unnamed", "negative control: listing a hidden client bypasses");
    settle(0);
    XDestroyWindow(app, shy); XSync(app, False);
    forget(0, shy);
    sleep_ms(50);

    /* 12b. Stacking: ADMIT lists images bottom to top. ws0 is w1 and w4 now. */
    XRaiseWindow(wm, w1); XSync(wm, False);
    sleep_ms(50);
    s = switch_ws(1, 1000, HOOK_NONE);
    expect(s, "ADMIT 2 cc00cc cc0000", "stacking: w1 raised above w4 is drawn last");
    settle(0);
    XRaiseWindow(wm, w4); XSync(wm, False);
    sleep_ms(50);
    s = switch_ws(1, 1000, HOOK_NONE);
    expect(s, "ADMIT 2 cc0000 cc00cc", "stacking: w4 raised above w1 is drawn last");
    settle(0);

    /* 12c. Compositor owner replaced while the scene holds every image. */
    commit_last = 1; hook_delay_ms = 100;
    sleep_ms(50);
    s = switch_ws(1, 1000, HOOK_COMP);
    expect(s, "BYPASS replaced", "ordered (commit last): compositor replacement cancels the scene");
    commit_last = 0; hook_delay_ms = 0;
    set_word(wm, D_COMP, XA_WINDOW, model_owner); XSync(wm, False);
    sleep_ms(50);
    check(capability(), "capability returns after the owner is restored");
    settle(0);

    /* 12d. Forced X error: another client frees one kept image before commit. */
    commit_last = 1;
    sleep_ms(50);
    s = switch_ws(1, 1000, HOOK_FREE);
    expect(s, "BYPASS xerror", "ordered (commit last): a freed kept image ends the scene");
    commit_last = 0;
    settle(0);

    /* 12e. Race: a member is destroyed before the model reads PREPARE. */
    Window gone = client(0, 0x00aaaa, 120, 90);
    sleep_ms(100);
    model_stop();
    s = switch_ws(1, 1000, HOOK_NONE);
    XDestroyWindow(app, gone); XSync(app, False);
    model_cont();
    expect(s, "BYPASS unnamed", "race: member destroyed before PREPARE is read bypasses");
    forget(0, gone);
    settle(0);

    /* 12. 64 admitted cycles return model resources to the baseline. */
    settle(1); settle(0);
    sleep_ms(100);
    uint64_t c0, b0, c1, b1;
    resources(model_owner, &c0, &b0);
    unsigned admitted = 0;
    for (unsigned i = 0; i < 64; i++) {
        s = switch_ws(current ? 0 : 1, 1000, HOOK_NONE);
        scene_line(s, got, sizeof(got));
        if (!strncmp(got, "ADMIT", 5)) admitted++;
        sleep_ms(20);
    }
    if (current) settle(0);
    sleep_ms(100);
    resources(model_owner, &c1, &b1);
    printf("METRIC: 64 cycles admitted=%u model XRes resources=%" PRIu64 "/%" PRIu64
           " pixmap_bytes=%" PRIu64 "/%" PRIu64 "\n", admitted, c0, c1, b0, b1);
    check(admitted == 64, "64 ordinary switches are all admitted");
    check(c1 == c0 && b1 == b0, "model resources return to baseline after 64 cycles");

    /* 13. Cleanup on cancellation and expiry while the scene holds images. */
    resources(model_owner, &c0, &b0);
    unsigned ended = 0;
    commit_last = 1; hook_delay_ms = 100;
    for (unsigned i = 0; i < 16; i++) {
        s = switch_ws(current ? 0 : 1, 1000, HOOK_EPOCH);
        scene_line(s, got, sizeof(got));
        if (!strcmp(got, "BYPASS epoch")) ended++;
    }
    commit_last = 0; hook_delay_ms = 0;
    for (unsigned i = 0; i < 2; i++) {
        s = switch_ws(current ? 0 : 1, 1000, HOOK_NO_COMMIT);
        scene_line(s, got, sizeof(got));
        if (!strcmp(got, "BYPASS expired")) ended++;
        set_word(wm, D_DESKTOP, XA_CARDINAL, current); XSync(wm, False);
    }
    if (current) settle(0);
    sleep_ms(100);
    resources(model_owner, &c1, &b1);
    printf("METRIC: 16 held-scene epoch cancels + 2 expiries ended=%u model XRes resources=%"
           PRIu64 "/%" PRIu64 " pixmap_bytes=%" PRIu64 "/%" PRIu64 "\n", ended, c0, c1, b0, b1);
    check(ended == 18, "held scenes end by epoch cancel or expiry");
    check(c1 == c0 && b1 == b0, "model resources return to baseline after cancels and expiries");

    /* Shutdown: no unexpected X error in the model. */
    kill(model_pid, SIGTERM);
    int errors = -1;
    while (model_line(line, sizeof(line), 3000))
        if (!strncmp(line, "ERRORS ", 7)) errors = atoi(line + 7);
    waitpid(model_pid, NULL, 0);
    check(errors == 0, "model saw no unexpected X error");
    XCloseDisplay(app); XCloseDisplay(wm);
    printf("%s: 3-P1 workspace capture prototype (%d failure(s))\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
