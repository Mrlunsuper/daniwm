/* Phase 3-P2 workspace crossfade prototype on a private Xvfb. Test-only.
 * "model" mode: rendering compositor model (manual redirection, overlay,
 * XRender) that keeps outgoing scenes with the 3-P1 rules and fades them.
 * default mode: simulated WM, app connections and XTest input. */
#define _POSIX_C_SOURCE 200809L
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/shape.h>
#include <X11/Xlib-xcb.h>
#include <xcb/xcb.h>
#include <xcb/res.h>
#include <assert.h>
#include <dlfcn.h>
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

#define WSR_QUERY_ATOM "_DANIWM_TEST_WS_QUERY_V1"       /* root ClientMessage */
#define WSR_RESERVED_ATOM "_DANIWM_TEST_WS_RESERVED_V1" /* root: close-layer bytes */
#define BG 0x202020UL

static int64_t mono_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
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
    int mapped, stationary, x, y;
    unsigned w, h, bw, depth;
    uint64_t generation;
    Pixmap pixmap;
    Picture picture;
    XRenderPictFormat *format;
    Damage damage;
} Track;
typedef struct {        /* per member, beside the 3-P1 scene state */
    Picture picture;
    XserverRegion shape; /* bounding shape in root coordinates */
    int x, y;
    unsigned w, h;
} Extra;
static Display *md;
static Window mroot, mowner, overlay;
static Track tracks[64];
static WSScene scene;
static Extra extra[WSP_MEMBERS];
static uint32_t latest_sequence;
static unsigned bpp_of[33], pad_of[33];
static int trapping, trapped, scene_xerror, dirty = 1, fading, replaced;
static int64_t fade_start;
static unsigned long frames, unexpected_errors;
static uint64_t peak_bytes;
static long duration_ms = 1000;
static int root_w, root_h, damage_event;
static Pixmap back;
static Picture back_pict, overlay_pict;
static XserverRegion monitors;
static volatile sig_atomic_t model_quit;
static Atom A_OWNER, A_EPOCH, A_TXN, A_EVENT, A_DESKTOP, A_QUERY, A_RESERVED, A_CM, A_TYPE, A_DOCK;

static int model_error(Display *d, XErrorEvent *e) {
    if (trapping) { trapped = 1; return 0; }
    /* A scene image freed by someone else: drop it, end the fade at loop top. */
    for (uint32_t i = 0; scene.phase != WSP_IDLE && i < scene.txn.count; i++) {
        if (extra[i].picture && e->resourceid == extra[i].picture) {
            extra[i].picture = 0; scene_xerror = 1; return 0;
        }
        if (scene.m[i].pixmap && e->resourceid == scene.m[i].pixmap) {
            scene.m[i].pixmap = 0; scene_xerror = 1; return 0;
        }
    }
    /* Live-window races (destroyed before DestroyNotify is read) are normal. */
    for (unsigned i = 0; i < 64; i++)
        if (tracks[i].id && (e->resourceid == tracks[i].id || e->resourceid == tracks[i].pixmap ||
            e->resourceid == tracks[i].picture || e->resourceid == tracks[i].damage)) {
            tracks[i].pixmap = 0; tracks[i].picture = 0; return 0;
        }
    char text[128];
    XGetErrorText(d, e->error_code, text, sizeof(text));
    fprintf(stderr, "model: unexpected X error %s req=%u/%u id=0x%lx\n", text,
            e->request_code, e->minor_code, e->resourceid);
    unexpected_errors++;
    return 0;
}
static Track *track(Window id, int create) {
    Track *slot = NULL;
    for (unsigned i = 0; i < 64; i++) {
        if (tracks[i].id == id) return &tracks[i];
        if (!tracks[i].id && !slot) slot = &tracks[i];
    }
    if (!create || !slot || id == overlay || id == mowner) return NULL;
    *slot = (Track){0};
    slot->id = id;
    return slot;
}
static void drop_live(Track *t) {
    if (t->picture) XRenderFreePicture(md, t->picture);
    if (t->pixmap) XFreePixmap(md, t->pixmap);
    t->picture = 0; t->pixmap = 0;
}
static int is_dock(Window id) {
    Atom type; int format; unsigned long n = 0, extra_bytes = 0;
    unsigned char *data = NULL;
    int dock = XGetWindowProperty(md, id, A_TYPE, 0, 8, False, XA_ATOM, &type, &format, &n,
        &extra_bytes, &data) == Success && data && n && ((Atom *)data)[0] == A_DOCK;
    if (data) XFree(data);
    return dock;
}
/* Name the window storage and wrap it in a Picture. Fails once unmapped. */
static void name_live(Track *t) {
    drop_live(t);
    XWindowAttributes a;
    trapping = 1; trapped = 0;
    Status ok = XGetWindowAttributes(md, t->id, &a);
    Pixmap pm = ok ? XCompositeNameWindowPixmap(md, t->id) : 0;
    XSync(md, False);
    if (trapped || !ok || a.map_state != IsViewable) {
        trapping = 0;
        if (pm && !trapped) XFreePixmap(md, pm);
        return;
    }
    t->format = XRenderFindVisualFormat(md, a.visual);
    XRenderPictureAttributes pa = {0};
    pa.subwindow_mode = IncludeInferiors;
    Picture p = XRenderCreatePicture(md, pm, t->format, CPSubwindowMode, &pa);
    XSync(md, False);
    trapping = 0;
    if (trapped) { XFreePixmap(md, pm); return; }
    t->pixmap = pm; t->picture = p;
    t->x = a.x; t->y = a.y; t->w = (unsigned)a.width; t->h = (unsigned)a.height;
    t->bw = (unsigned)a.border_width; t->depth = (unsigned)a.depth;
    t->stationary = a.override_redirect || is_dock(t->id);
}
static void read_monitors(void) {
    int n = 0, users = 0;
    XRRMonitorInfo *m = XRRGetMonitors(md, mroot, True, &n);
    for (int i = 0; i < n; i++) if (!m[i].automatic) users++;
    XRectangle rects[16]; int nr = 0;
    /* Test-only: Xvfb cannot delete its automatic monitor, so prefer test monitors. */
    for (int i = 0; i < n && nr < 16; i++) if (!users || !m[i].automatic)
        rects[nr++] = (XRectangle){(short)m[i].x, (short)m[i].y,
            (unsigned short)m[i].width, (unsigned short)m[i].height};
    if (m) XRRFreeMonitors(m);
    if (monitors) XFixesDestroyRegion(md, monitors);
    monitors = XFixesCreateRegion(md, rects, nr);
}
static void line(const char *fmt, uint32_t seq, const char *rest) {
    printf(fmt, seq, rest);
    fflush(stdout);
}
static void scene_free(void) {
    for (uint32_t i = 0; i < WSP_MEMBERS; i++) {
        if (extra[i].picture) XRenderFreePicture(md, extra[i].picture);
        if (extra[i].shape) XFixesDestroyRegion(md, extra[i].shape);
        extra[i] = (Extra){0};
    }
    wsp_clear(&scene, (WSRelease)NULL);
}
static void release_members(void) {
    for (uint32_t i = 0; i < WSP_MEMBERS; i++)
        if (scene.m[i].pixmap) { XFreePixmap(md, scene.m[i].pixmap); scene.m[i].pixmap = 0; }
}
/* End a held or fading scene. A fade end always repaints the exact final frame. */
static void scene_end(int reason) {
    if (scene.phase == WSP_IDLE) return;
    if (fading) line("FADE %" PRIu32 " END %s\n", scene.txn.sequence, wsp_reason_names[reason]);
    else line("SCENE %" PRIu32 " BYPASS %s\n", scene.txn.sequence, wsp_reason_names[reason]);
    release_members();
    scene_free();
    fading = 0;
    dirty = 1;
}
static void record_ranks(void) {
    Window r, p, *kids = NULL; unsigned nk = 0;
    if (XQueryTree(md, mroot, &r, &p, &kids, &nk))
        for (unsigned k = 0; k < nk; k++) {
            int index = wsp_member(&scene, (uint32_t)kids[k]);
            if (index >= 0) scene.m[index].rank = k;
        }
    if (kids) XFree(kids);
}
static void admit(void) {
    /* Restacks before the unmaps count: ranks are read again when complete. */
    record_ranks();
    for (uint32_t i = 0; i < scene.txn.count; i++) {
        Track *t = track(scene.m[i].id, 0);
        XRenderPictFormat *f = t && t->format ? t->format :
            XRenderFindStandardFormat(md, PictStandardRGB24);
        trapping = 1; trapped = 0;
        extra[i].picture = XRenderCreatePicture(md, scene.m[i].pixmap, f, 0, NULL);
        XSync(md, False);
        trapping = 0;
        if (trapped) { extra[i].picture = 0; scene_end(WSP_XERROR); return; }
    }
    char pictures[256] = "";
    for (uint32_t i = 0; i < scene.txn.count; i++) {
        size_t used = strlen(pictures);
        snprintf(pictures + used, sizeof(pictures) - used, " 0x%lx", extra[i].picture);
    }
    printf("SCENE %" PRIu32 " ADMIT %" PRIu32 "%s\n", scene.txn.sequence, scene.txn.count, pictures);
    fflush(stdout);
    if (scene.bytes > peak_bytes) peak_bytes = scene.bytes;
    if (!scene.txn.count) {
        line("FADE %" PRIu32 " DONE%s\n", scene.txn.sequence, "");
        scene_free();
        return;
    }
    fading = 1;
    fade_start = mono_ms();
    dirty = 1;
}
static void prepare(const XClientMessageEvent *e) {
    uint32_t sequence = (uint32_t)e->data.l[2];
    Window owner = (Window)prop_word(md, mroot, A_OWNER, XA_WINDOW);
    if (!owner || (Window)e->data.l[1] != owner || !wsp_newer(sequence, latest_sequence)) {
        line("SCENE %" PRIu32 " BYPASS %s\n", sequence, "stale"); return;
    }
    latest_sequence = sequence;
    /* Newest wins, also for a fade in progress. */
    scene_end(WSP_SUPERSEDED);
    uint32_t epoch = (uint32_t)prop_word(md, mroot, A_EPOCH, XA_CARDINAL);
    Atom type; int format; unsigned long n = 0, more = 0;
    unsigned char *data = NULL;
    WSTxn t;
    int ok = XGetWindowProperty(md, mroot, A_TXN, 0, WSP_WORDS, False, XA_CARDINAL, &type,
        &format, &n, &more, &data) == Success && type == XA_CARDINAL && format == 32 &&
        wsp_decode(&t, (unsigned long *)data, n, more, (uint32_t)owner, (uint32_t)mowner, epoch);
    if (data) XFree(data);
    if (!ok || t.sequence != sequence) {
        line("SCENE %" PRIu32 " BYPASS %s\n", sequence, ok ? "overwritten" : "invalid"); return;
    }
    if (!wsp_age_ok(t.sent_ms, (uint32_t)mono_ms(), t.deadline_ms)) {
        line("SCENE %" PRIu32 " BYPASS %s\n", sequence, "late"); return;
    }
    uint64_t bytes[WSP_MEMBERS] = {0}, gens[WSP_MEMBERS] = {0}, total = 0;
    for (uint32_t i = 0; i < t.count; i++) {
        Track *k = track(t.members[i], 0);
        if (!k || !k->mapped || !k->pixmap || k->depth > 32 ||
            !wsp_image_bytes(&bytes[i], k->w + 2 * k->bw, k->h + 2 * k->bw,
                bpp_of[k->depth], pad_of[k->depth])) {
            line("SCENE %" PRIu32 " BYPASS %s\n", sequence, "unnamed"); return;
        }
        gens[i] = k->generation;
        total += bytes[i];
    }
    uint64_t reserved = prop_word(md, mroot, A_RESERVED, XA_CARDINAL);
    if (!wsp_shared_fits(reserved, total) || !wsp_begin(&scene, &t, bytes, gens, mono_ms())) {
        line("SCENE %" PRIu32 " BYPASS %s\n", sequence, "budget"); return;
    }
    for (uint32_t i = 0; i < t.count; i++) {
        Track *k = track(t.members[i], 0);
        extra[i].x = k->x; extra[i].y = k->y;
        extra[i].w = k->w + 2 * k->bw; extra[i].h = k->h + 2 * k->bw;
        trapping = 1; trapped = 0;
        extra[i].shape = XFixesCreateRegionFromWindow(md, k->id, WindowRegionBounding);
        XFixesTranslateRegion(md, extra[i].shape, k->x + (int)k->bw, k->y + (int)k->bw);
        XSync(md, False);
        trapping = 0;
        if (trapped) { extra[i].shape = 0; scene_end(WSP_UNNAMED); return; }
    }
    record_ranks();
}
static void draw(Picture src, int x, int y, unsigned w, unsigned h, XserverRegion clip, double alpha) {
    if (!src) return;
    XFixesSetPictureClipRegion(md, back_pict, 0, 0, clip);
    Picture mask = None;
    if (alpha < 1.0) {
        XRenderColor c = {0, 0, 0, (unsigned short)(alpha * 0xffff)};
        mask = XRenderCreateSolidFill(md, &c);
    }
    XRenderComposite(md, PictOpOver, src, mask, back_pict, 0, 0, 0, 0, x, y, w, h);
    if (mask) XRenderFreePicture(md, mask);
}
static void draw_live(Track *t) {
    XserverRegion r = XFixesCreateRegionFromWindow(md, t->id, WindowRegionBounding);
    XFixesTranslateRegion(md, r, t->x + (int)t->bw, t->y + (int)t->bw);
    draw(t->picture, t->x, t->y, t->w + 2 * t->bw, t->h + 2 * t->bw, r, 1.0);
    XFixesDestroyRegion(md, r);
}
static void paint(int64_t now) {
    XRenderColor bg = {0x2020, 0x2020, 0x2020, 0xffff};
    XFixesSetPictureClipRegion(md, back_pict, 0, 0, None);
    XRenderFillRectangle(md, PictOpSrc, back_pict, &bg, 0, 0, (unsigned)root_w, (unsigned)root_h);
    Window r, p, *kids = NULL; unsigned nk = 0;
    XQueryTree(md, mroot, &r, &p, &kids, &nk);
    for (unsigned k = 0; k < nk; k++) {
        Track *t = track(kids[k], 0);
        if (t && t->mapped && t->picture && !t->stationary) draw_live(t);
    }
    if (fading) {
        double progress = (double)(now - fade_start) / (double)duration_ms;
        if (progress > 1.0) progress = 1.0;
        unsigned order[WSP_MEMBERS];
        wsp_stack_order(&scene, order);
        XserverRegion clip = XFixesCreateRegion(md, NULL, 0);
        for (uint32_t i = 0; i < scene.txn.count; i++) {
            Extra *x = &extra[order[i]];
            XFixesIntersectRegion(md, clip, x->shape, monitors);
            draw(x->picture, x->x, x->y, x->w, x->h, clip, 1.0 - progress);
        }
        XFixesDestroyRegion(md, clip);
    }
    for (unsigned k = 0; k < nk; k++) {
        Track *t = track(kids[k], 0);
        if (t && t->mapped && t->picture && t->stationary) draw_live(t);
    }
    if (kids) XFree(kids);
    XFixesSetPictureClipRegion(md, back_pict, 0, 0, None);
    XRenderComposite(md, PictOpSrc, back_pict, None, overlay_pict, 0, 0, 0, 0, 0, 0,
        (unsigned)root_w, (unsigned)root_h);
    frames++;
    dirty = 0;
}
static void unmap_member(Track *t) {
    int index = wsp_member(&scene, (uint32_t)t->id);
    if (index < 0 || scene.phase != WSP_COLLECTING) return;
    if (!wsp_retain(&scene, index, t->pixmap, t->generation)) { scene_end(WSP_UNNAMED); return; }
    /* The member image now belongs to the scene. */
    if (t->picture) XRenderFreePicture(md, t->picture);
    t->picture = 0; t->pixmap = 0;
    if (scene.phase == WSP_READY && scene.committed) admit();
}
static void model_event(XEvent *ev) {
    if (ev->type == damage_event + XDamageNotify) {
        XDamageSubtract(md, ((XDamageNotifyEvent *)ev)->damage, None, None);
        dirty = 1;
        return;
    }
    switch (ev->type) {
    case MapNotify: {
        if (ev->xmap.event != mroot) break;
        Track *t = track(ev->xmap.window, 1);
        if (!t) break;
        t->mapped = 1; t->generation++;
        name_live(t);
        if (!t->damage) {
            trapping = 1;
            t->damage = XDamageCreate(md, t->id, XDamageReportNonEmpty);
            XSync(md, False);
            trapping = 0;
        }
        dirty = 1;
        break;
    }
    case ConfigureNotify: {
        if (ev->xconfigure.window == mroot) break;
        Track *t = track(ev->xconfigure.window, 0);
        if (!t) break;
        int resized = (unsigned)ev->xconfigure.width != t->w || (unsigned)ev->xconfigure.height != t->h;
        t->x = ev->xconfigure.x; t->y = ev->xconfigure.y;
        t->w = (unsigned)ev->xconfigure.width; t->h = (unsigned)ev->xconfigure.height;
        if (resized) { if (t->mapped) name_live(t); else drop_live(t); }
        dirty = 1;
        break;
    }
    case UnmapNotify: {
        if (ev->xunmap.event != mroot || ev->xunmap.send_event) break;
        Track *t = track(ev->xunmap.window, 0);
        if (!t) break;
        unmap_member(t);
        t->mapped = 0;
        drop_live(t);
        dirty = 1;
        break;
    }
    case DestroyNotify: {
        if (ev->xdestroywindow.event != mroot) break;
        Track *t = track(ev->xdestroywindow.window, 0);
        if (t) { drop_live(t); *t = (Track){0}; }
        dirty = 1;
        break;
    }
    case PropertyNotify:
        if (ev->xproperty.window != mroot) break;
        if (ev->xproperty.atom == A_OWNER) scene_end(WSP_OWNER);
        else if (ev->xproperty.atom == A_EPOCH) { read_monitors(); scene_end(WSP_EPOCH); dirty = 1; }
        else if (ev->xproperty.atom == A_DESKTOP && scene.phase != WSP_IDLE && !fading &&
                 prop_word(md, mroot, A_DESKTOP, XA_CARDINAL) == scene.txn.to) {
            if (scene.phase == WSP_READY) admit(); else scene.committed = 1;
        }
        break;
    case ClientMessage:
        if (ev->xclient.window != mroot || ev->xclient.format != 32) break;
        if (ev->xclient.message_type == A_EVENT && ev->xclient.data.l[0] == WSP_VERSION)
            prepare(&ev->xclient);
        else if (ev->xclient.message_type == A_QUERY) {
            printf("STAT %ld frames=%lu peak=%" PRIu64 " fading=%d\n", ev->xclient.data.l[0],
                   frames, peak_bytes, fading);
            fflush(stdout);
        }
        break;
    case SelectionClear:
        if (ev->xselectionclear.selection == A_CM) { scene_end(WSP_REPLACED); replaced = 1; }
        break;
    }
}
static void on_term(int sig) { (void)sig; model_quit = 1; }
static int model_main(const char *display, long duration) {
    duration_ms = duration;
    md = XOpenDisplay(display);
    if (!md) return 2;
    XSetErrorHandler(model_error);
    signal(SIGTERM, on_term);
    mroot = DefaultRootWindow(md);
    root_w = DisplayWidth(md, DefaultScreen(md)); root_h = DisplayHeight(md, DefaultScreen(md));
    A_OWNER = XInternAtom(md, WSP_OWNER_ATOM, False);
    A_EPOCH = XInternAtom(md, WSP_EPOCH_ATOM, False);
    A_TXN = XInternAtom(md, WSP_TXN_ATOM, False);
    A_EVENT = XInternAtom(md, WSP_EVENT_ATOM, False);
    A_DESKTOP = XInternAtom(md, "_NET_CURRENT_DESKTOP", False);
    A_QUERY = XInternAtom(md, WSR_QUERY_ATOM, False);
    A_RESERVED = XInternAtom(md, WSR_RESERVED_ATOM, False);
    A_CM = XInternAtom(md, "_NET_WM_CM_S0", False);
    A_TYPE = XInternAtom(md, "_NET_WM_WINDOW_TYPE", False);
    A_DOCK = XInternAtom(md, "_NET_WM_WINDOW_TYPE_DOCK", False);
    int nf, derr;
    XPixmapFormatValues *f = XListPixmapFormats(md, &nf);
    for (int i = 0; f && i < nf; i++) if (f[i].depth > 0 && f[i].depth <= 32) {
        bpp_of[f[i].depth] = (unsigned)f[i].bits_per_pixel;
        pad_of[f[i].depth] = (unsigned)f[i].scanline_pad;
    }
    if (f) XFree(f);
    XDamageQueryExtension(md, &damage_event, &derr);
    XSetWindowAttributes sa = {0}; sa.override_redirect = True;
    mowner = XCreateWindow(md, mroot, -10, -10, 1, 1, 0, CopyFromParent, InputOnly,
        CopyFromParent, CWOverrideRedirect, &sa);
    XSetSelectionOwner(md, A_CM, mowner, CurrentTime);
    if (XGetSelectionOwner(md, A_CM) != mowner) return 3;
    XGrabServer(md); /* consistent startup snapshot only; never during a switch */
    XCompositeRedirectSubwindows(md, mroot, CompositeRedirectManual);
    XSelectInput(md, mroot, SubstructureNotifyMask | PropertyChangeMask);
    overlay = XCompositeGetOverlayWindow(md, mroot);
    XserverRegion empty = XFixesCreateRegion(md, NULL, 0);
    XFixesSetWindowShapeRegion(md, overlay, ShapeInput, 0, 0, empty);
    XFixesDestroyRegion(md, empty);
    Window r, p, *kids = NULL; unsigned nk = 0;
    XQueryTree(md, mroot, &r, &p, &kids, &nk);
    for (unsigned i = 0; i < nk; i++) {
        XWindowAttributes a;
        if (!XGetWindowAttributes(md, kids[i], &a) || a.class != InputOutput ||
            a.map_state != IsViewable) continue;
        Track *t = track(kids[i], 1);
        if (!t) continue;
        t->mapped = 1; t->generation = 1;
        name_live(t);
        t->damage = XDamageCreate(md, t->id, XDamageReportNonEmpty);
    }
    if (kids) XFree(kids);
    XUngrabServer(md);
    XWindowAttributes ra;
    XGetWindowAttributes(md, mroot, &ra);
    XRenderPictFormat *rf = XRenderFindVisualFormat(md, ra.visual);
    back = XCreatePixmap(md, mroot, (unsigned)root_w, (unsigned)root_h, (unsigned)ra.depth);
    back_pict = XRenderCreatePicture(md, back, rf, 0, NULL);
    overlay_pict = XRenderCreatePicture(md, overlay, rf, 0, NULL);
    read_monitors();
    unsigned long version = WSP_VERSION;
    XChangeProperty(md, mowner, XInternAtom(md, WSP_CAP_ATOM, False), XA_CARDINAL, 32,
        PropModeReplace, (unsigned char *)&version, 1);
    XChangeProperty(md, mroot, XInternAtom(md, WSP_COMP_ATOM, False), XA_WINDOW, 32,
        PropModeReplace, (unsigned char *)&mowner, 1);
    paint(mono_ms());
    XSync(md, False);
    printf("READY 0x%lx\n", mowner);
    fflush(stdout);
    int fd = ConnectionNumber(md);
    while (!model_quit && !replaced) {
        while (XPending(md) && !replaced) {
            XEvent ev; XNextEvent(md, &ev); model_event(&ev);
            if (scene_xerror) { scene_xerror = 0; scene_end(WSP_XERROR); }
        }
        int64_t now = mono_ms();
        if (!fading && wsp_expired(&scene, now)) scene_end(WSP_EXPIRED);
        if (fading && now - fade_start >= duration_ms) {
            line("FADE %" PRIu32 " DONE%s\n", scene.txn.sequence, "");
            release_members();
            scene_free();
            fading = 0;
            dirty = 1;
        }
        if (dirty || fading) {
            paint(now);
            XSync(md, False); /* async Render errors surface before the next check */
            if (scene_xerror) { scene_xerror = 0; scene_end(WSP_XERROR); paint(mono_ms()); }
        }
        fd_set set; FD_ZERO(&set); FD_SET(fd, &set);
        struct timeval tv = {0, fading ? 8000 : 20000};
        if (!XPending(md)) select(fd + 1, &set, NULL, NULL, &tv);
    }
    /* Clean teardown: give the screen back to the server. */
    scene_end(WSP_CANCELLED);
    for (unsigned i = 0; i < 64; i++) if (tracks[i].id) drop_live(&tracks[i]);
    XRenderFreePicture(md, back_pict); XRenderFreePicture(md, overlay_pict);
    XFreePixmap(md, back);
    if (monitors) XFixesDestroyRegion(md, monitors);
    XCompositeUnredirectSubwindows(md, mroot, CompositeRedirectManual);
    XCompositeReleaseOverlayWindow(md, mroot);
    XSync(md, False);
    printf("ERRORS %lu\n", unexpected_errors);
    fflush(stdout);
    XCloseDisplay(md);
    return 0;
}

/* ----------------------------------------------------------------- driver */
#define NWS 4
static Display *wm, *app;
static Window root, wm_owner, model_owner;
static Window ws[NWS][8];
static unsigned nws[NWS], current;
static uint32_t sequence, epoch = 1;
static pid_t model_pid;
static FILE *model_out;
static int failures;
static Atom D_OWNER, D_EPOCH, D_TXN, D_EVENT, D_DESKTOP, D_COMP, D_CAP, D_QUERY, D_RESERVED;
static int (*fake_motion)(Display *, int, int, int, unsigned long);
static int (*fake_button)(Display *, unsigned int, Bool, unsigned long);
static int (*fake_key)(Display *, unsigned int, Bool, unsigned long);
enum { HOOK_NONE, HOOK_RESTACK };
static Window restack_target;

static void check(int ok, const char *what) {
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    fflush(stdout);
    if (!ok) failures++;
}
static void set_word(Atom atom, Atom type, unsigned long value) {
    XChangeProperty(wm, root, atom, type, 32, PropModeReplace, (unsigned char *)&value, 1);
}
static unsigned long pixel(int x, int y) {
    XImage *image = XGetImage(wm, root, x, y, 1, 1, AllPlanes, ZPixmap);
    assert(image);
    unsigned long v = XGetPixel(image, 0, 0) & 0xffffff;
    XDestroyImage(image);
    return v;
}
static unsigned channel(unsigned long p, int shift) { return (unsigned)(p >> shift) & 255; }
static Window client(unsigned slot, unsigned long color, int x, int y, unsigned w, unsigned h) {
    Window win = XCreateSimpleWindow(app, root, x, y, w, h, 0, 0, color);
    XSelectInput(app, win, ButtonPressMask | KeyPressMask | EnterWindowMask);
    XSync(app, False);
    ws[slot][nws[slot]++] = win;
    if (slot == current) { XMapWindow(wm, win); XSync(wm, False); }
    return win;
}
static int model_line(char *out, size_t size, int timeout_ms) {
    struct pollfd p = {fileno(model_out), POLLIN, 0};
    int left = timeout_ms;
    if (poll(&p, 1, left) <= 0) return 0;
    if (!fgets(out, (int)size, model_out)) return 0;
    out[strcspn(out, "\n")] = 0;
    return 1;
}
/* Wait for a model line that starts with prefix; keep its rest. */
static int wait_line(const char *prefix, char *rest, size_t size, int timeout_ms) {
    char l[512];
    int64_t end = mono_ms() + timeout_ms;
    while (mono_ms() < end) {
        if (!model_line(l, sizeof(l), (int)(end - mono_ms()))) break;
        if (!strncmp(l, prefix, strlen(prefix))) { snprintf(rest, size, "%s", l + strlen(prefix)); return 1; }
    }
    snprintf(rest, size, "(no report)");
    return 0;
}
static int scene_result(uint32_t seq, char *rest, size_t size) {
    char prefix[48];
    snprintf(prefix, sizeof(prefix), "SCENE %" PRIu32 " ", seq);
    return wait_line(prefix, rest, size, 3000);
}
static int fade_result(uint32_t seq, char *rest, size_t size) {
    char prefix[48];
    snprintf(prefix, sizeof(prefix), "FADE %" PRIu32 " ", seq);
    return wait_line(prefix, rest, size, 3000);
}
static uint64_t last_peak;
static unsigned long stat_frames(void) {
    static long id;
    XEvent e = {0};
    e.xclient.type = ClientMessage; e.xclient.window = root;
    e.xclient.message_type = D_QUERY; e.xclient.format = 32; e.xclient.data.l[0] = ++id;
    XSendEvent(wm, root, False, SubstructureNotifyMask, &e);
    XSync(wm, False);
    char rest[256], prefix[32];
    snprintf(prefix, sizeof(prefix), "STAT %ld ", id);
    if (!wait_line(prefix, rest, sizeof(rest), 2000)) return ~0UL;
    last_peak = strtoull(strstr(rest, "peak=") + 5, NULL, 10);
    return strtoul(strstr(rest, "frames=") + 7, NULL, 10);
}
/* CompositeClientWindow records follow the number of root children (the
 * driver creates owner windows), so they are reported apart. */
static unsigned redirect_records;
static void resources(uint64_t *count, uint64_t *bytes) {
    Atom per_window = XInternAtom(wm, "CompositeClientWindow", False);
    xcb_connection_t *c = XGetXCBConnection(wm);
    XSync(wm, False);
    xcb_res_query_client_resources_reply_t *r = xcb_res_query_client_resources_reply(c,
        xcb_res_query_client_resources(c, (uint32_t)model_owner), NULL);
    assert(r);
    *count = 0;
    for (xcb_res_type_iterator_t it = xcb_res_query_client_resources_types_iterator(r);
         it.rem; xcb_res_type_next(&it)) {
        if (it.data->resource_type == per_window) redirect_records = it.data->count;
        else *count += it.data->count;
    }
    free(r);
    xcb_res_query_client_pixmap_bytes_reply_t *p = xcb_res_query_client_pixmap_bytes_reply(c,
        xcb_res_query_client_pixmap_bytes(c, (uint32_t)model_owner), NULL);
    assert(p);
    *bytes = ((uint64_t)p->bytes_overflow << 32) | p->bytes;
    free(p);
}
/* daniwm view() order: PREPARE, desktop, unmaps, maps, focus. Never waits. */
static uint32_t switch_ws(unsigned to, int hook) {
    unsigned from = current;
    uint32_t seq = ++sequence;
    WSTxn t = {0};
    t.wm = (uint32_t)wm_owner; t.compositor = (uint32_t)model_owner; t.epoch = epoch;
    t.sequence = seq; t.from = from; t.to = to; t.sent_ms = (uint32_t)mono_ms();
    t.deadline_ms = 1000;
    for (unsigned i = 0; i < nws[from]; i++) t.members[t.count++] = (uint32_t)ws[from][i];
    unsigned long words[WSP_WORDS];
    size_t n = wsp_encode(words, &t);
    XChangeProperty(wm, root, D_TXN, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)words, (int)n);
    XEvent e = {0};
    e.xclient.type = ClientMessage; e.xclient.window = root;
    e.xclient.message_type = D_EVENT; e.xclient.format = 32;
    e.xclient.data.l[0] = WSP_VERSION; e.xclient.data.l[1] = (long)wm_owner;
    e.xclient.data.l[2] = (long)seq; e.xclient.data.l[3] = WSP_PREPARE;
    XSendEvent(wm, root, False, SubstructureNotifyMask, &e);
    set_word(D_DESKTOP, XA_CARDINAL, to);
    if (hook == HOOK_RESTACK) {
        /* Test ordering control: the model reads PREPARE before the restack. */
        XSync(wm, False); sleep_ms(100);
        XRaiseWindow(wm, restack_target);
    }
    for (unsigned i = 0; i < nws[from]; i++) XUnmapWindow(wm, ws[from][i]);
    for (unsigned i = 0; i < nws[to]; i++) XMapWindow(wm, ws[to][i]);
    if (nws[to]) XSetInputFocus(wm, ws[to][0], RevertToPointerRoot, CurrentTime);
    else XSetInputFocus(wm, PointerRoot, RevertToPointerRoot, CurrentTime);
    XSync(wm, False);
    current = to;
    return seq;
}
static void finish(uint32_t seq) {
    char rest[256];
    if (scene_result(seq, rest, sizeof(rest)) && !strncmp(rest, "ADMIT", 5))
        fade_result(seq, rest, sizeof(rest));
    sleep_ms(30);
}
static void forget(unsigned slot, Window win) {
    for (unsigned i = 0; i < nws[slot]; i++)
        if (ws[slot][i] == win) { ws[slot][i] = ws[slot][--nws[slot]]; return; }
}
static void drain_app(void) { while (XPending(app)) { XEvent e; XNextEvent(app, &e); } }
static void count_app(Window w, unsigned *buttons, unsigned *keys, unsigned *enters) {
    *buttons = *keys = *enters = 0;
    XSync(app, False);
    XEvent e;
    while (XCheckWindowEvent(app, w, ButtonPressMask | KeyPressMask | EnterWindowMask, &e)) {
        if (e.type == ButtonPress) (*buttons)++;
        if (e.type == KeyPress) (*keys)++;
        if (e.type == EnterNotify) (*enters)++;
    }
}
int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "model")) return model_main(argv[2], atol(argv[3]));
    assert(argc == 3);
    const char *display = argv[2];
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
    int input = fake_motion && fake_button && fake_key;
    if (!input) puts("BLOCKED: input checks need runtime libXtst.so.6");
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
    D_QUERY = XInternAtom(wm, WSR_QUERY_ATOM, False);
    D_RESERVED = XInternAtom(wm, WSR_RESERVED_ATOM, False);
    wm_owner = XCreateSimpleWindow(wm, root, -5, -5, 1, 1, 0, 0, 0);
    set_word(D_OWNER, XA_WINDOW, wm_owner);
    set_word(D_EPOCH, XA_CARDINAL, epoch);
    set_word(D_DESKTOP, XA_CARDINAL, 0);
    /* Two unequal test monitors with a gap at x 800..900. */
    XRRMonitorInfo *m = XRRAllocateMonitor(wm, 0);
    m->name = XInternAtom(wm, "TEST-A", False); m->x = 0; m->y = 0; m->width = 800; m->height = 600;
    m->mwidth = 200; m->mheight = 150; m->primary = True;
    XRRSetMonitor(wm, root, m);
    m->name = XInternAtom(wm, "TEST-B", False); m->x = 900; m->width = 640; m->height = 480; m->primary = False;
    XRRSetMonitor(wm, root, m);
    XSync(wm, False);

    /* Workspaces. ws0 and ws1 cover the same rectangle on monitor A. */
    Window a = client(0, 0xcc0000, 100, 100, 500, 400);      /* red */
    Window a2 = client(0, 0xff8800, 300, 300, 400, 250);     /* orange, above a */
    Window span = client(0, 0xffffff, 700, 520, 300, 60);    /* white, over the gap */
    Window neg = client(0, 0xcccc00, -50, 540, 200, 50);     /* yellow, negative x */
    Window b = client(1, 0x0000cc, 100, 100, 500, 400);      /* blue */
    Window c = client(1, 0x00cc00, 1000, 100, 400, 300);     /* green, monitor B */
    Window w2 = client(2, 0x00cccc, 100, 100, 500, 400);     /* cyan */
    (void)span; (void)neg; (void)w2; (void)c;
    /* ARGB member (50 % red, premultiplied) and a shaped member, ws0, monitor B. */
    XVisualInfo vi;
    assert(XMatchVisualInfo(app, DefaultScreen(app), 32, TrueColor, &vi));
    XSetWindowAttributes swa = {0};
    swa.colormap = XCreateColormap(app, root, vi.visual, AllocNone);
    swa.background_pixel = 0x80800000UL; swa.border_pixel = 0;
    Window argb = XCreateWindow(app, root, 1000, 420, 100, 50, 0, 32, InputOutput, vi.visual,
        CWColormap | CWBackPixel | CWBorderPixel, &swa);
    Window shaped = XCreateSimpleWindow(app, root, 1150, 150, 200, 100, 0, 0, 0xffffff);
    XRectangle half = {0, 0, 100, 100};
    XShapeCombineRectangles(app, shaped, ShapeBounding, 0, 0, &half, 1, ShapeSet, YXBanded);
    XSync(app, False);
    ws[0][nws[0]++] = argb; ws[0][nws[0]++] = shaped;
    XMapWindow(wm, argb); XMapWindow(wm, shaped);
    /* Dock: stationary UI on both workspaces, never a member. */
    swa.override_redirect = True; swa.background_pixel = 0xff00ff;
    Window dock = XCreateWindow(app, root, 0, 0, 800, 30, 0, CopyFromParent, InputOutput,
        CopyFromParent, CWOverrideRedirect | CWBackPixel, &swa);
    XMapWindow(app, dock);
    XSync(app, False); XSync(wm, False);

    /* Start the rendering model. */
    int fds[2];
    assert(pipe(fds) == 0);
    model_pid = fork();
    assert(model_pid >= 0);
    if (!model_pid) {
        dup2(fds[1], STDOUT_FILENO); close(fds[0]); close(fds[1]);
        close(ConnectionNumber(wm)); close(ConnectionNumber(app));
        execl("/proc/self/exe", argv[0], "model", display, "1000", (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    model_out = fdopen(fds[0], "r");
    setvbuf(model_out, NULL, _IONBF, 0);
    char rest[512], fade_end[64];
    assert(wait_line("READY ", rest, sizeof(rest), 3000));
    model_owner = (Window)strtoul(rest, NULL, 16);
    sleep_ms(200);
    check(pixel(150, 150) == 0xcc0000 && pixel(400, 15) == 0xff00ff,
          "model draws the current workspace and the dock");
    /* Warm-up: every client is mapped once, so per-window Damage objects exist. */
    finish(switch_ws(1, HOOK_NONE)); finish(switch_ws(2, HOOK_NONE)); finish(switch_ws(0, HOOK_NONE));
    sleep_ms(100);
    uint64_t c0, b0, c1, b1;
    resources(&c0, &b0);
    unsigned records0 = redirect_records;
    if (input) { fake_motion(wm, -1, 150, 150, 0); XSync(wm, False); sleep_ms(50); }
    drain_app();

    /* 1-3. One crossfade ws0 -> ws1: progression, dock, gap, shape, ARGB, input. */
    unsigned long f0 = stat_frames();
    int64_t t0 = mono_ms();
    uint32_t s = switch_ws(1, HOOK_NONE);
    scene_result(s, rest, sizeof(rest));
    check(!strncmp(rest, "ADMIT 6", 7), "crossfade ws0->ws1 admitted with 6 members");
    sleep_ms(40);
    unsigned long early = pixel(150, 150), early_dock = pixel(400, 15);
    unsigned long early_gap = pixel(850, 550), early_shape_hole = pixel(1300, 200);
    unsigned long early_shape = pixel(1200, 200), early_argb = pixel(1050, 445);
    unsigned long early_neg = pixel(50, 560);
    int64_t click_ms = 0;
    unsigned long click_pixel = 0;
    if (input) {
        click_pixel = pixel(150, 150);
        click_ms = mono_ms() - t0;
        fake_button(wm, 1, True, 0); fake_button(wm, 1, False, 0);
        fake_button(wm, 4, True, 0); fake_button(wm, 4, False, 0);
        unsigned key = XKeysymToKeycode(wm, XK_a);
        fake_key(wm, key, True, 0); fake_key(wm, key, False, 0);
        XSync(wm, False);
    }
    printf("METRIC: t=%" PRId64 " ms pixels early=%06lx dock=%06lx gap=%06lx shape=%06lx "
           "shape_hole=%06lx argb=%06lx negx=%06lx\n", mono_ms() - t0, early, early_dock,
           early_gap, early_shape, early_shape_hole, early_argb, early_neg);
    check(channel(early, 16) > 0x90 && channel(early, 0) < 0x40,
          "early frame shows mostly the outgoing image (red over blue)");
    check(early_dock == 0xff00ff, "dock stays at full colour during the fade (early)");
    check(early_gap == BG, "gap between monitors shows no scene pixels");
    check(early_shape_hole == 0x00cc00, "shaped member hole shows the incoming client exactly");
    check(channel(early_shape, 16) > 0x90 && channel(early_shape, 8) > 0x90,
          "shaped member body fades from its own colour");
    check(channel(early_argb, 16) > 0x60 && channel(early_argb, 16) < 0xb0,
          "ARGB member blends with its own alpha (not opaque, not missing)");
    check(channel(early_neg, 16) > 0x90 && channel(early_neg, 8) > 0x90,
          "member with negative x is drawn in its visible part");
    sleep_ms(450);
    unsigned long mid = pixel(150, 150), mid_dock = pixel(400, 15);
    printf("METRIC: t=%" PRId64 " ms mid=%06lx dock=%06lx\n", mono_ms() - t0, mid, mid_dock);
    check(channel(mid, 16) > 0x30 && channel(mid, 16) < 0xa0 &&
          channel(mid, 0) > 0x30 && channel(mid, 0) < 0xa0, "middle frame is a blend");
    check(mid_dock == 0xff00ff, "dock stays at full colour during the fade (middle)");
    /* Ambiguity: how long the outgoing image dominates the probe pixel. */
    int64_t dominated_until = -1;
    for (int i = 0; i < 80; i++) {
        unsigned long p = pixel(150, 150);
        if (channel(p, 16) <= channel(p, 0)) { dominated_until = mono_ms() - t0; break; }
        sleep_ms(10);
    }
    fade_result(s, rest, sizeof(rest));
    check(!strcmp(rest, "DONE"), "fade reports DONE");
    sleep_ms(30);
    check(pixel(150, 150) == 0x0000cc && pixel(1200, 200) == 0x00cc00 && pixel(850, 550) == BG,
          "exact final frame after the fade");
    if (input) {
        unsigned ab, ak, ae, bb, bk, be;
        count_app(a, &ab, &ak, &ae);
        count_app(b, &bb, &bk, &be);
        printf("METRIC: input at t=%" PRId64 " ms over pixel %06lx (outgoing red dominates): "
               "incoming b buttons=%u keys=%u enters=%u; outgoing a buttons=%u keys=%u enters=%u\n",
               click_ms, click_pixel, bb, bk, be, ab, ak, ae);
        printf("METRIC: outgoing image dominates the probe pixel until t=%" PRId64 " ms\n",
               dominated_until);
        check(bb == 2 && bk == 1 && ab == 0 && ak == 0,
              "click, wheel and key go to the incoming client while the outgoing image shows");
        check(be >= 1, "stationary pointer: incoming client gets EnterNotify at the switch");
    }
    unsigned long f1 = stat_frames();
    sleep_ms(500);
    unsigned long f2 = stat_frames();
    printf("METRIC: frames during fade=%lu, after fade in 500 ms=%lu\n", f1 - f0, f2 - f1);
    check(f2 == f1, "no frames after the fade ends");

    /* 4. Stalled model: input still goes to the incoming client. */
    drain_app();
    s = switch_ws(0, HOOK_NONE);
    scene_result(s, rest, sizeof(rest));
    sleep_ms(150);
    kill(model_pid, SIGSTOP);
    unsigned long frozen1 = pixel(150, 150);
    if (input) { fake_button(wm, 1, True, 0); fake_button(wm, 1, False, 0); XSync(wm, False); }
    sleep_ms(1200);
    unsigned long frozen2 = pixel(150, 150);
    kill(model_pid, SIGCONT);
    int64_t resumed = mono_ms();
    unsigned long after = 0;
    while (mono_ms() - resumed < 500) { after = pixel(150, 150); if (after == 0xcc0000) break; sleep_ms(5); }
    int64_t catch_up = mono_ms() - resumed;
    fade_result(s, rest, sizeof(rest));
    printf("METRIC: stalled frame %06lx -> %06lx over 1200 ms; final after resume in %" PRId64 " ms\n",
           frozen1, frozen2, catch_up);
    check(frozen1 == frozen2 && channel(frozen1, 0) > 0x80, "stalled model freezes the outgoing (blue) frame");
    check(after == 0xcc0000 && catch_up < 100, "exact final frame within 100 ms after resume");
    if (input) {
        unsigned ab, ak, ae, bb, bk, be;
        count_app(a, &ab, &ak, &ae);
        count_app(b, &bb, &bk, &be);
        check(ab == 1 && bb == 0, "click during the stall goes to the incoming client");
    }
    sleep_ms(100);

    /* 5. Mid-fade cancels: monitor change with epoch, owner change. */
    s = switch_ws(1, HOOK_NONE);
    scene_result(s, rest, sizeof(rest));
    sleep_ms(200);
    XRRDeleteMonitor(wm, root, XInternAtom(wm, "TEST-B", False));
    m->name = XInternAtom(wm, "TEST-B2", False); m->x = 900; m->width = 500; m->height = 480;
    XRRSetMonitor(wm, root, m);
    set_word(D_EPOCH, XA_CARDINAL, ++epoch); XSync(wm, False);
    fade_result(s, rest, sizeof(rest));
    sleep_ms(50);
    check(!strcmp(rest, "END epoch") && pixel(150, 150) == 0x0000cc,
          "monitor change without root resize (epoch) ends the fade on the exact final frame");
    s = switch_ws(0, HOOK_NONE);
    scene_result(s, rest, sizeof(rest));
    sleep_ms(200);
    wm_owner = XCreateSimpleWindow(wm, root, -5, -5, 1, 1, 0, 0, 0);
    set_word(D_OWNER, XA_WINDOW, wm_owner); XSync(wm, False);
    fade_result(s, rest, sizeof(rest));
    sleep_ms(50);
    check(!strcmp(rest, "END owner") && pixel(150, 150) == 0xcc0000,
          "owner change ends the fade on the exact final frame");
    f1 = stat_frames(); sleep_ms(300); f2 = stat_frames();
    check(f2 == f1, "no frames after a cancel");

    /* 6. Restack between PREPARE and the unmaps: ranks are read when complete. */
    restack_target = a;  /* a is below a2; raise it */
    s = switch_ws(1, HOOK_RESTACK);
    scene_result(s, rest, sizeof(rest));
    sleep_ms(40);
    unsigned long overlap = pixel(400, 400);
    fade_result(s, rest, sizeof(rest));
    printf("METRIC: overlap pixel after restack %06lx\n", overlap);
    check(channel(overlap, 16) > 0x90 && channel(overlap, 8) < 0x40,
          "restack before the unmaps: raised member (red) is drawn on top");
    XRaiseWindow(wm, a2); XSync(wm, False);

    /* 7. Storm A->B->C->A every 30 ms: newest wins, one scene at most. */
    sleep_ms(100);
    uint32_t storm[6];
    unsigned order[6] = {2, 0, 1, 2, 0, 1};
    for (unsigned i = 0; i < 6; i++) { storm[i] = switch_ws(order[i], HOOK_NONE); sleep_ms(30); }
    unsigned superseded = 0, done_last = 0;
    for (unsigned i = 0; i < 6; i++) {
        scene_result(storm[i], rest, sizeof(rest));
        if (!strncmp(rest, "ADMIT", 5)) {
            fade_result(storm[i], rest, sizeof(rest));
            if (!strcmp(rest, "END superseded")) superseded++;
            if (i == 5 && !strcmp(rest, "DONE")) done_last = 1;
        } else if (!strcmp(rest, "BYPASS superseded")) superseded++;
    }
    sleep_ms(50);
    printf("METRIC: storm superseded=%u last_done=%u\n", superseded, done_last);
    check(superseded >= 3, "storm: older switches end as superseded (newest wins)");
    check(pixel(150, 150) == 0x0000cc, "storm: exact final frame of the last workspace");
    stat_frames();
    printf("METRIC: peak scene bytes=%" PRIu64 " (budget %" PRIu64 ")\n", last_peak, WSP_BUDGET);
    check(last_peak > 0 && last_peak <= WSP_BUDGET, "peak scene bytes stay within the budget");

    /* 8. Shared budget: a simulated close-layer reserve leaves no room. */
    set_word(D_RESERVED, XA_CARDINAL, (unsigned long)(WSP_BUDGET - 4096)); XSync(wm, False);
    s = switch_ws(0, HOOK_NONE);
    scene_result(s, rest, sizeof(rest));
    check(!strcmp(rest, "BYPASS budget"), "shared budget with close layers bypasses");
    set_word(D_RESERVED, XA_CARDINAL, 0); XSync(wm, False);
    sleep_ms(50);
    check(pixel(150, 150) == 0xcc0000, "bypass shows the final frame at once");

    /* 9a. Close during an active fade: an outgoing member and an incoming client die. */
    Window gone_out = client(0, 0x8800ff, 650, 150, 100, 100);  /* purple, mapped (ws0) */
    Window gone_in = client(1, 0x00ff88, 650, 150, 100, 100);   /* incoming, same place */
    sleep_ms(100);
    s = switch_ws(1, HOOK_NONE);
    scene_result(s, rest, sizeof(rest));
    sleep_ms(200);
    XDestroyWindow(app, gone_out); XDestroyWindow(app, gone_in); XSync(app, False);
    sleep_ms(50);
    unsigned long kept = pixel(700, 200);
    fade_result(s, fade_end, sizeof(fade_end));
    sleep_ms(30);
    printf("METRIC: destroyed during fade: kept pixel %06lx, end %s\n", kept, fade_end);
    check(channel(kept, 0) > 0xa0 && channel(kept, 8) < 0x40,
          "destroyed outgoing member: kept image stays drawn until DONE");
    check(!strcmp(fade_end, "DONE") && pixel(700, 200) == BG,
          "destroyed incoming client: exact final frame shows what lies under it");
    forget(0, gone_out); forget(1, gone_in);
    s = switch_ws(0, HOOK_NONE);
    finish(s);

    /* 9. Asynchronous X error: another client frees a scene Picture mid-fade. */
    s = switch_ws(1, HOOK_NONE);
    scene_result(s, rest, sizeof(rest));
    Picture victim = 0;
    char *hex = strstr(rest, "0x");
    if (hex) victim = (Picture)strtoul(hex, NULL, 16);
    sleep_ms(100);
    if (victim) { XRenderFreePicture(wm, victim); XSync(wm, False); }
    fade_result(s, rest, sizeof(rest));
    sleep_ms(50);
    check(victim && !strcmp(rest, "END xerror") && pixel(150, 150) == 0x0000cc,
          "asynchronous error on a scene Picture ends the fade on the exact final frame");

    /* 10. Empty outgoing workspace: no fade frames. */
    s = switch_ws(3, HOOK_NONE);  /* ws1 -> ws3 (empty incoming) fades ws1 out */
    finish(s);
    sleep_ms(100);
    f1 = stat_frames();
    s = switch_ws(0, HOOK_NONE);  /* ws3 (empty) -> ws0 */
    scene_result(s, rest, sizeof(rest));
    char fade[64];
    fade_result(s, fade, sizeof(fade));
    sleep_ms(200);
    f2 = stat_frames();
    printf("METRIC: empty outgoing: %s / %s, frames=%lu\n", rest, fade, f2 - f1);
    check(!strcmp(rest, "ADMIT 0") && !strcmp(fade, "DONE") && f2 - f1 < 10,
          "empty outgoing workspace: admitted with no fade frames");

    /* Resources back to the baseline after fades, cancels, storm and errors. */
    sleep_ms(200);
    resources(&c1, &b1);
    printf("METRIC: model XRes resources=%" PRIu64 "/%" PRIu64 " pixmap_bytes=%" PRIu64 "/%" PRIu64
           " (CompositeClientWindow %u/%u; +1 = the new WM owner window)\n",
           c0, c1, b0, b1, records0, redirect_records);
    check(c1 == c0 && b1 == b0, "model resources return to the baseline");

    /* 11. Second compositor takes _NET_WM_CM_S0 mid-fade. */
    s = switch_ws(1, HOOK_NONE);
    scene_result(s, rest, sizeof(rest));
    sleep_ms(150);
    Window rival = XCreateSimpleWindow(wm, root, -5, -5, 1, 1, 0, 0, 0);
    XSetSelectionOwner(wm, XInternAtom(wm, "_NET_WM_CM_S0", False), rival, CurrentTime);
    XSync(wm, False);
    fade_result(s, rest, sizeof(rest));
    int errors = -1;
    char l[256];
    while (model_line(l, sizeof(l), 3000)) if (!strncmp(l, "ERRORS ", 7)) { errors = atoi(l + 7); break; }
    waitpid(model_pid, NULL, 0);
    sleep_ms(100);
    check(!strcmp(rest, "END replaced"), "second compositor owner ends the fade");
    check(pixel(150, 150) == 0x0000cc, "after the model leaves, the server shows the final workspace");
    check(errors == 0, "model saw no unexpected X error");
    XCloseDisplay(app); XCloseDisplay(wm);
    printf("%s: 3-P2 workspace crossfade prototype (%d failure(s))\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
