/* Phase 0 only: private-display baseline and retained-image experiment.
 * Includes the real compositor for its error traps, fade and repaint paths.
 * No new code is linked into either production binary. */
#define main comp_main
#include "../src/comp.c"
#undef main
#include "animation-snapshot.h"
#include <X11/Xutil.h>
#include <assert.h>
#include <stdint.h>
#ifdef WITH_XCB_RES
#include <xcb/xcb.h>
#include <xcb/res.h>
#endif

static unsigned images_created, images_released;
/* Fault injection is test-local; production error handling is untouched. */
static unsigned capture_failure;

typedef struct {
    uint64_t pixmap_bytes, resources;
} ResourceSample;
#ifdef WITH_XCB_RES
static xcb_connection_t *resource_connection;
static Window resource_anchor;
static ResourceSample resource_sample(void) {
    if (!resource_connection) return (ResourceSample){0};
    XSync(dpy, False);
    xcb_generic_error_t *error = NULL;
    xcb_res_query_client_resources_reply_t *r = xcb_res_query_client_resources_reply(
        resource_connection, xcb_res_query_client_resources(resource_connection,
        (uint32_t)resource_anchor), &error);
    assert(r && !error);
    ResourceSample sample = {0};
    xcb_res_type_iterator_t types = xcb_res_query_client_resources_types_iterator(r);
    for (; types.rem; xcb_res_type_next(&types)) sample.resources += types.data->count;
    free(r);
    xcb_res_query_client_pixmap_bytes_reply_t *p = xcb_res_query_client_pixmap_bytes_reply(
        resource_connection, xcb_res_query_client_pixmap_bytes(resource_connection,
        (uint32_t)resource_anchor), &error);
    assert(p && !error);
    sample.pixmap_bytes = ((uint64_t)p->bytes_overflow << 32) | p->bytes;
    free(p);
    return sample;
}
static int resources_init(const char *display) {
    resource_connection = xcb_connect(display, NULL);
    assert(!xcb_connection_has_error(resource_connection));
    const xcb_query_extension_reply_t *ext = xcb_get_extension_data(resource_connection, &xcb_res_id);
    if (!ext || !ext->present) {
        xcb_disconnect(resource_connection);
        resource_connection = NULL;
        puts("SKIP: X-Resource server extension unavailable");
        return 0;
    }
    resource_anchor = XCreateSimpleWindow(dpy, root, 0, 0, 1, 1, 0, 0, 0);
    return 1;
}
#else
static ResourceSample resource_sample(void) { return (ResourceSample){0}; }
static int resources_init(const char *display) {
    (void)display;
    puts("SKIP: X-Resource measurements need xcb-res development files");
    return 0;
}
#endif

static void resources_equal(ResourceSample before, ResourceSample after) {
    assert(before.resources == after.resources);
    assert(before.pixmap_bytes == after.pixmap_bytes);
}

static void release_image(void *context, SnapshotImage image) {
    (void)context;
    trap_errors();
    if (image.picture) XRenderFreePicture(dpy, image.picture);
    if (image.pixmap) { XFreePixmap(dpy, image.pixmap); images_released++; }
    assert(!untrap_errors());
}

/* Staged checks matter: XCreate* returns nonzero even on asynchronous error.
 * Capture copies while viewable, never queries after the client has gone.
 * Every Picture here is fresh; transform/filter/clip state is not shared. */
static int capture_image(void *context, SnapshotKey key, unsigned width,
                         unsigned height, unsigned depth, SnapshotImage *out) {
    (void)context;
    XWindowAttributes a;
    trap_errors();
    int valid = XGetWindowAttributes(dpy, (Window)key.identity, &a);
    int error = untrap_errors();
    if (error || !valid || a.map_state != IsViewable || depth != 32 ||
        (unsigned)(a.width + 2 * a.border_width) != width ||
        (unsigned)(a.height + 2 * a.border_width) != height) return 0;
    XRenderPictFormat *sf = XRenderFindVisualFormat(dpy, a.visual);
    XRenderPictFormat *df = XRenderFindStandardFormat(dpy, PictStandardARGB32);
    if (!sf || !df) return 0;
    trap_errors();
    Pixmap source = XCompositeNameWindowPixmap(dpy, (Window)key.identity);
    error = untrap_errors();
    if (error || !source) return 0;
    Picture sp = None;
    XRectangle *rects = NULL;
    int ok = 0;
    Window rr; int x, y;
    unsigned pw, ph, border, dep;
    trap_errors();
    valid = XGetGeometry(dpy, source, &rr, &x, &y, &pw, &ph, &border, &dep);
    error = untrap_errors();
    if (error || !valid || pw != width || ph != height || dep != (unsigned)a.depth) goto done;
    trap_errors();
    Pixmap dst = XCreatePixmap(dpy, root, capture_failure == 1 ? 0 : width, height, depth);
    error = untrap_errors();
    if (error || !dst) goto done;
    out->pixmap = dst;
    images_created++;
    trap_errors();
    /* Root has depth 24 on this private fixture: incompatible with ARGB32. */
    Picture dp = XRenderCreatePicture(dpy, capture_failure == 2 ? root : dst, df, 0, NULL);
    error = untrap_errors();
    if (error || !dp) goto done;
    out->picture = dp;
    trap_errors();
    Picture candidate = XRenderCreatePicture(dpy, source, sf, 0, NULL);
    error = untrap_errors();
    if (error || !candidate) goto done;
    sp = candidate;
    Bool shaped, clip_shaped;
    int bx, by, cx, cy, n = 0, ordering;
    unsigned bw, bh, cw, ch;
    trap_errors();
    valid = XShapeQueryExtents(dpy, (Window)key.identity, &shaped, &bx, &by,
                              &bw, &bh, &clip_shaped, &cx, &cy, &cw, &ch);
    if (valid && shaped)
        rects = XShapeGetRectangles(dpy, (Window)key.identity,
                                   ShapeBounding, &n, &ordering);
    error = untrap_errors();
    if (error || !valid || n < 0 || (shaped && n && !rects)) goto done;
    trap_errors();
    /* Explicitly restore defaults before copy, including the destination
     * clip (source clipping on redirected pixmaps is not portable). */
    XTransform identity = {{{65536, 0, 0}, {0, 65536, 0}, {0, 0, 65536}}};
    XRenderPictureAttributes pa = {0};
    pa.clip_mask = None;
    XRenderSetPictureTransform(dpy, sp, &identity);
    XRenderSetPictureFilter(dpy, sp, FilterNearest, NULL, 0);
    XRenderChangePicture(dpy, sp, CPClipMask, &pa);
    XRenderChangePicture(dpy, dp, CPClipMask, &pa);
    XRenderColor clear = {0, 0, 0, 0};
    XRenderFillRectangle(dpy, PictOpSrc, dp, &clear, 0, 0, width, height);
    if (shaped) {
        for (int i = 0; i < n; i++) {
            rects[i].x = (short)(rects[i].x + a.border_width);
            rects[i].y = (short)(rects[i].y + a.border_width);
        }
        XRenderSetPictureClipRectangles(dpy, dp, 0, 0, rects, n);
    }
    XRenderComposite(dpy, capture_failure == 3 ? 255 : PictOpSrc,
        sp, None, dp, 0, 0, 0, 0, 0, 0, width, height);
    XRenderChangePicture(dpy, dp, CPClipMask, &pa);
    ok = !untrap_errors();
done:
    if (rects) XFree(rects);
    trap_errors();
    if (sp) XRenderFreePicture(dpy, sp);
    XFreePixmap(dpy, source);
    if (untrap_errors()) ok = 0;
    return ok;
}

static unsigned long pixel(Drawable drawable, int x, int y) {
    XImage *image = XGetImage(dpy, drawable, x, y, 1, 1, AllPlanes, ZPixmap);
    assert(image);
    unsigned long value = XGetPixel(image, 0, 0);
    XDestroyImage(image);
    return value;
}

static int take(SnapshotPool *pool, SnapshotKey key, int64_t now) {
    XWindowAttributes a;
    assert(XGetWindowAttributes(dpy, (Window)key.identity, &a));
    int n;
    XPixmapFormatValues *formats = XListPixmapFormats(dpy, &n);
    assert(formats);
    unsigned bpp = 0, pad = 0;
    /* Retain ARGB32 even for opaque sources: shaped holes must remain
     * transparent, not opaque black. Budget the destination's real stride. */
    for (int i = 0; i < n; i++) if (formats[i].depth == 32) {
        bpp = (unsigned)formats[i].bits_per_pixel;
        pad = (unsigned)formats[i].scanline_pad;
    }
    XFree(formats);
    return snapshot_take(pool, key, (unsigned)(a.width + 2 * a.border_width),
                         (unsigned)(a.height + 2 * a.border_width),
                         32, bpp, pad, now);
}

static Window client_window(Display *owner, unsigned border) {
    Window window = XCreateSimpleWindow(owner, DefaultRootWindow(owner),
        40, 40, 200, 120, border, 0xffffff, 0x336699);
    XMapWindow(owner, window);
    XSync(owner, False);
    return window;
}

static void baseline_math(void) {
    opt_fade = 1; opt_fade_ms = 160; opt_shadow = 0; opt_dim = 1;
    Win sample = {0};
    Win *w = &sample;
    w->depth = 24;
    w->opacity = OPAQUE;
    long long start = now_ms();
    w->born = start;
    w->id = 42; w->generation = 1;
    opening_start(w, start);
    fading = 0;
    assert(win_alpha(w, start) == 0.0 && fading);
    assert(win_alpha(w, start + 80) == 0.5);
    fading = 0;
    assert(win_alpha(w, start + 160) == 1.0 && !fading);
    opening_start(w, start);
    assert(!win_is_opaque(w, start));
    assert(win_is_opaque(w, start + 160));
    w->opacity = OPAQUE / 2;
    assert(!win_is_opaque(w, start + 160));
    double alpha = win_alpha(w, start + 80);
    assert(alpha > 0.249 && alpha < 0.251);
    w->opacity = OPAQUE;
    opt_fade_ms = 0;
    fading = 0;
    assert(win_alpha(w, start) == 1.0 && !fading);
    opt_fade_ms = 160; opt_fade = 0;
    assert(win_alpha(w, start) == 1.0 && !fading);
    opt_fade = 1; w->override = 1;
    assert(win_alpha(w, start) == 1.0 && !fading);
    w->override = 0;
    w->depth = 32;
    assert(!win_is_opaque(w, start + 160));
    w->depth = 24; w->shaped = 1;
    assert(!win_is_opaque(w, start + 160));
    w->shaped = 0; w->id = 42;
    opt_dim = 0.92;
    assert(win_dim(w) == 0.92);
    active_win = 42;
    assert(win_dim(w) == 1.0);
    active_win = None; w->fullscreen = 1;
    assert(win_dim(w) == 1.0);
    opt_dim = 1;
    puts("PASS: existing fade endpoints, opacity product, zero/no-fade, override, ARGB/shape occlusion and dim");
}

static void baseline(Window window) {
    opt_fade = opt_shadow = 0; opt_dim = 1;
    assert(win_add(window));
    /* Deterministic render workload, not a refresh-rate benchmark. */
    long long durations[32], sum = 0;
    for (unsigned i = 0; i < 32; i++) {
        dirty = full_dirty = 1;
        long long before = now_ms();
        repaint();
        XSync(dpy, False);
        durations[i] = now_ms() - before;
        sum += durations[i];
    }
    assert(pixel(back_buf, 55, 55) == 0x336699);
    assert(!dirty && !fading);
    for (unsigned i = 0; i < 1000; i++) assert(!repaint_due());
    for (unsigned i = 0; i < 32; i++) for (unsigned j = i + 1; j < 32; j++)
        if (durations[j] < durations[i]) {
            long long tmp = durations[i]; durations[i] = durations[j]; durations[j] = tmp;
        }
    printf("METRIC: copy backend 32 forced full repaints, mean_ms=%.3f p50_ms=%lld "
           "p95_ms=%lld max_ms=%lld resolution=%dx%d frame_limit_ms=%d\n",
           (double)sum / 32, durations[15], durations[30], durations[31],
           back_w, back_h, FRAME_INTERVAL_MS);
    puts("PASS: 1000 settled repaint_due checks request no animation paint (not an idle-wakeup measurement)");
    win_del(window);
}

int main(int argc, char **argv) {
    /* Baseline arithmetic needs no X connection or fabricated event loop. */
    if (argc == 2 && !strcmp(argv[1], "--self-test")) {
        baseline_math();
        return 0;
    }
    /* Never inherit DISPLAY. The wrapper supplies its newly spawned server. */
    if (argc != 2 || strncmp(argv[1], ":", 1)) {
        fprintf(stderr, "usage: comp-animation --self-test | :PRIVATE_DISPLAY\n");
        return 2;
    }
    dpy = XOpenDisplay(argv[1]);
    assert(dpy);
    Display *owner = XOpenDisplay(argv[1]);
    assert(owner);
    screen = DefaultScreen(dpy); root = RootWindow(dpy, screen);
    int ev, err;
    assert(XCompositeQueryExtension(dpy, &ev, &err));
    assert(XDamageQueryExtension(dpy, &dmg_event, &dmg_error));
    assert(XRenderQueryExtension(dpy, &ev, &render_errbase));
    shape_ok = XShapeQueryExtension(dpy, &shape_event, &err);
    assert(shape_ok);
    XSetErrorHandler(xerror_ignore);
    trap_errors();
    XCompositeRedirectSubwindows(dpy, root, CompositeRedirectManual);
    assert(!untrap_errors());
    int have_resources = resources_init(argv[1]);
    Window window = client_window(owner, 0);
    baseline(window);
    XDestroyWindow(owner, window); XSync(owner, False);

    SnapshotPool pool;
    snapshot_init(&pool, capture_image, release_image, NULL);
    ResourceSample lifecycle_before = resource_sample();
    window = client_window(owner, 2);
    SnapshotKey key = {window, 1};
    assert(take(&pool, key, now_ms()));
    const Snapshot *s = snapshot_get(&pool, key, now_ms());
    assert(s && s->width == 204 && s->height == 124);
    assert(pixel(s->image.pixmap, 0, 0) == 0xffffffff);
    assert(pixel(s->image.pixmap, 10, 10) == 0xff336699);
    XResizeWindow(owner, window, 300, 200); XSync(owner, False);
    assert(pixel(s->image.pixmap, 10, 10) == 0xff336699);
    XUnmapWindow(owner, window); XSync(owner, False);
    assert(pixel(s->image.pixmap, 10, 10) == 0xff336699);
    assert(!take(&pool, (SnapshotKey){window, 2}, now_ms()));
    XDestroyWindow(owner, window); XSync(owner, False);
    assert(pixel(s->image.pixmap, 10, 10) == 0xff336699);
    /* Expected BadWindow: callback must return failure without publishing XIDs. */
    assert(!snapshot_take(&pool, (SnapshotKey){window, 2}, 204, 124,
                          32, 32, 32, now_ms()));
    snapshot_drop(&pool, key);
    assert(!pool.count && images_created == images_released);
    puts("PASS: copied border/content survives resize, unmap and destroy; hidden/dead capture fails closed");

    for (unsigned i = 0; i < 128; i++) {
        window = client_window(owner, 0);
        XRectangle shape = {0, 0, 80, 60};
        XShapeCombineRectangles(owner, window, ShapeBounding, 0, 0, &shape, 1, ShapeSet, Unsorted);
        XSync(owner, False);
        key = (SnapshotKey){window, i + 2};
        int64_t captured = now_ms();
        assert(take(&pool, key, captured));
        s = snapshot_get(&pool, key, captured);
        assert(s && pixel(s->image.pixmap, 10, 10) == 0xff336699);
        assert(pixel(s->image.pixmap, 100, 100) == 0);
        XDestroyWindow(owner, window); XSync(owner, False);
        if (i % 2) snapshot_expire(&pool, captured + SNAPSHOT_TTL_MS);
        else snapshot_drop(&pool, key);
        assert(!pool.count && !pool.bytes && images_created == images_released);
    }
    puts("PASS: 128 shaped lifecycle cycles; holes cleared, drop/TTL release every owned image");

    XVisualInfo info;
    if (XMatchVisualInfo(owner, DefaultScreen(owner), 32, TrueColor, &info)) {
        XSetWindowAttributes a = {0};
        a.colormap = XCreateColormap(owner, DefaultRootWindow(owner), info.visual, AllocNone);
        a.background_pixel = 0x80402010; a.border_pixel = 0;
        window = XCreateWindow(owner, DefaultRootWindow(owner), 20, 20, 32, 32,
            0, 32, InputOutput, info.visual, CWColormap | CWBackPixel | CWBorderPixel, &a);
        XMapWindow(owner, window); XSync(owner, False);
        key = (SnapshotKey){window, 131};
        assert(take(&pool, key, now_ms()));
        s = snapshot_get(&pool, key, now_ms());
        assert(s && pixel(s->image.pixmap, 10, 10) == 0x80402010);
        XDestroyWindow(owner, window); XSync(owner, False);
        assert(pixel(s->image.pixmap, 10, 10) == 0x80402010);
        snapshot_clear(&pool);
        XFreeColormap(owner, a.colormap);
        puts("PASS: premultiplied ARGB bytes survive capture and source destruction");
    } else puts("SKIP: private server lacks a 32-bit TrueColor visual");
    snapshot_clear(&pool);
    if (have_resources) {
        ResourceSample after = resource_sample();
        resources_equal(lifecycle_before, after);
        printf("METRIC: XRes lifecycle baseline/final resources=%llu/%llu "
               "pixmap_bytes=%llu/%llu\n",
               (unsigned long long)lifecycle_before.resources, (unsigned long long)after.resources,
               (unsigned long long)lifecycle_before.pixmap_bytes, (unsigned long long)after.pixmap_bytes);
    }
    /* Exercise actual server allocations, not just mock admission failures. */
    window = client_window(owner, 0);
    ResourceSample before = resource_sample();
    for (unsigned i = 0; i < SNAPSHOT_COUNT; i++)
        assert(take(&pool, (SnapshotKey){window, 200 + i}, now_ms()));
    assert(pool.count == SNAPSHOT_COUNT);
    unsigned created = images_created;
    assert(!take(&pool, (SnapshotKey){window, 300}, now_ms()));
    assert(images_created == created);
    snapshot_clear(&pool);
    if (have_resources) resources_equal(before, resource_sample());
    for (capture_failure = 1; capture_failure <= 3; capture_failure++) {
        assert(!take(&pool, (SnapshotKey){window, 400 + capture_failure}, now_ms()));
        assert(!pool.count && !pool.bytes && images_created == images_released);
        if (have_resources) resources_equal(before, resource_sample());
    }
    capture_failure = 0;
    XResizeWindow(owner, window, 4096, 2048); XSync(owner, False);
    before = resource_sample();
    key = (SnapshotKey){window, 500};
    int64_t captured = now_ms();
    assert(take(&pool, key, captured));
    assert(pool.bytes == SNAPSHOT_BYTES);
    if (have_resources) {
        ResourceSample full = resource_sample();
        assert(full.resources == before.resources + 2);
        assert(full.pixmap_bytes == before.pixmap_bytes + SNAPSHOT_BYTES);
        printf("METRIC: XRes full-budget resource_delta=%llu pixmap_byte_delta=%llu\n",
               (unsigned long long)(full.resources - before.resources),
               (unsigned long long)(full.pixmap_bytes - before.pixmap_bytes));
    }
    XResizeWindow(owner, window, 200, 120); XSync(owner, False);
    created = images_created;
    assert(!take(&pool, (SnapshotKey){window, 501}, captured));
    assert(images_created == created);
    snapshot_expire(&pool, captured + SNAPSHOT_TTL_MS);
    assert(!pool.count && !pool.bytes);
    /* Source storage changed above; compare against its new settled baseline. */
    before = resource_sample();
    for (unsigned i = 0; i < 64; i++) {
        assert(take(&pool, (SnapshotKey){window, 600 + i}, now_ms()));
        snapshot_clear(&pool);
        if (have_resources) resources_equal(before, resource_sample());
    }
    XDestroyWindow(owner, window); XSync(owner, False);
    if (have_resources) resources_equal(lifecycle_before, resource_sample());
    puts("PASS: real count/byte admission, expiry, 3 asynchronous X failures and 64 resource-stable cycles");
    assert(!pool.count && !pool.bytes && images_created == images_released);
    printf("METRIC: copied_images=%u released_images=%u peak_count=%u "
           "peak_pixel_bytes=%zu limits=%u/%zu/%dms\n", images_created,
           images_released, pool.peak_count, pool.peak_bytes,
           SNAPSHOT_COUNT, SNAPSHOT_BYTES, SNAPSHOT_TTL_MS);
#ifdef WITH_XCB_RES
    if (resource_connection) {
        XDestroyWindow(dpy, resource_anchor);
        xcb_disconnect(resource_connection);
    }
#endif
    XCloseDisplay(owner);
    XCompositeUnredirectSubwindows(dpy, root, CompositeRedirectManual);
    XCloseDisplay(dpy);
    return 0;
}
