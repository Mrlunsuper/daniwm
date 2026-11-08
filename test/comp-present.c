/* Exercise the real repaint path, but withhold Present events like a GPU
 * retaining flip buffers. Xvfb alone normally returns Idle immediately. */
#define XPresentPixmap test_present_pixmap
#define main comp_main
#include "../src/comp.c"
#undef main
#undef XPresentPixmap
#include <assert.h>
#include <X11/Xutil.h>

static int submissions;
static Pixmap submitted;
static uint32_t submitted_serial;

void test_present_pixmap(Display *display, Window window, Pixmap pixmap,
    uint32_t serial, XserverRegion valid, XserverRegion update,
    int x_off, int y_off, RRCrtc crtc, XSyncFence wait, XSyncFence idle,
    uint32_t options, uint64_t msc, uint64_t divisor, uint64_t remainder,
    XPresentNotify *notifies, int nnotifies) {
    (void)valid; (void)update; (void)crtc; (void)wait; (void)idle;
    (void)options; (void)msc; (void)divisor; (void)remainder;
    (void)notifies; (void)nnotifies;
    assert(pixmap != back_buf);
    assert(x_off == 0 && y_off == 0);
    for (int i = 0; i < PRESENT_BUFFERS; i++)
        if (present_bufs[i].pixmap == pixmap) assert(!present_bufs[i].busy);
    submitted = pixmap;
    submitted_serial = serial;
    submissions++;
    /* Simulate a full flip, NOT just the update bbox: this catches stale
     * pixels outside the damage rectangle when slots alternate. */
    XCopyArea(display, pixmap, window, present_gc, 0, 0,
        (unsigned)back_w, (unsigned)back_h, 0, 0);
}

static unsigned long get_pixel(Drawable drawable, int x, int y) {
    XImage *image = XGetImage(dpy, drawable, x, y, 1, 1, AllPlanes, ZPixmap);
    assert(image);
    unsigned long color = XGetPixel(image, 0, 0);
    XDestroyImage(image);
    return color;
}

static void complete(uint32_t serial) {
    XPresentCompleteNotifyEvent event = {0};
    event.eid = (uint32_t)present_eid;
    event.window = root;
    event.kind = PresentCompleteKindPixmap;
    event.mode = PresentCompleteModeFlip;
    event.serial_number = serial;
    present_complete(&event);
}

static void partial_frame(Window window, GC gc, int x, unsigned long color) {
    XSetForeground(dpy, gc, color);
    XFillRectangle(dpy, window, gc, x, 10, 20, 20);
    Win *w = win_get(window);
    assert(w);
    dmg_add(w->x + x, w->y + 10, w->x + x + 20, w->y + 30);
    dirty = 1;
    last_paint = now_ms() - 17;
    assert(repaint_due());
    repaint();
}

int main(void) {
    dpy = XOpenDisplay(NULL);
    assert(dpy);
    screen = DefaultScreen(dpy);
    root = RootWindow(dpy, screen);
    assert(XDamageQueryExtension(dpy, &dmg_event, &dmg_error));
    int event;
    assert(XRenderQueryExtension(dpy, &event, &render_errbase));
    XSetErrorHandler(xerror_ignore);
    XCompositeRedirectSubwindows(dpy, root, CompositeRedirectManual);
    opt_fade = opt_shadow = 0;
    opt_dim = 1;
    present_ok = 1;
    present_eid = 42;
    Window window = XCreateSimpleWindow(dpy, root, 40, 40, 200, 120, 0, 0, 0x336699);
    GC gc = XCreateGC(dpy, window, 0, NULL);
    XMapWindow(dpy, window);
    XSync(dpy, False);
    assert(win_add(window));
    repaint();
    assert(submissions == 1 && present_pending);
    Pixmap first = submitted;
    uint32_t first_serial = submitted_serial;
    assert(get_pixel(first, 55, 55) == 0x336699);

    /* A wrong serial/kind/eid/window cannot complete the current request. */
    complete(first_serial + 1);
    assert(present_pending);
    XPresentCompleteNotifyEvent other = {0};
    other.eid = (uint32_t)present_eid;
    other.window = root;
    other.serial_number = first_serial;
    other.kind = PresentCompleteKindNotifyMSC;
    present_complete(&other);
    assert(present_pending);
    other.kind = PresentCompleteKindPixmap;
    other.eid++;
    present_complete(&other);
    assert(present_pending);
    other.eid--;
    other.window = window;
    present_complete(&other);
    assert(present_pending);
    complete(first_serial);
    assert(!present_pending && present_bufs[0].busy);
    dirty = 1;
    present_bufs[0].sent_ms = now_ms() - 1000;
    assert(present_wait_ms(now_ms()) == -1); /* old scanout + free slots is normal */
    assert(present_ok);
    puts("PASS: Complete does not release scanout buffer; stale events ignored");

    partial_frame(window, gc, 10, 0xff0000);
    assert(submitted != first);
    assert(get_pixel(first, 55, 55) == 0x336699);
    assert(get_pixel(submitted, 55, 55) == 0xff0000);
    assert(get_pixel(submitted, 95, 55) == 0x336699);
    uint32_t second_serial = submitted_serial;
    complete(first_serial); /* late duplicate must not clear request 2 */
    assert(present_pending);
    complete(second_serial);
    partial_frame(window, gc, 50, 0x00ff00);
    assert(submissions == 3);
    assert(get_pixel(submitted, 55, 55) == 0xff0000);
    assert(get_pixel(submitted, 95, 55) == 0x00ff00);
    assert(get_pixel(root, 55, 55) == 0xff0000);
    complete(submitted_serial);
    assert(present_free_slot() < 0);
    puts("PASS: full snapshots preserve earlier partial damage across 3 slots");

    XPresentIdleNotifyEvent idle = {0};
    idle.eid = (uint32_t)present_eid;
    idle.window = root;
    idle.pixmap = first;
    idle.serial_number = first_serial + 1;
    present_buffer_idle(&idle);
    assert(present_free_slot() < 0);
    idle.serial_number = first_serial;
    idle.eid++;
    present_buffer_idle(&idle);
    assert(present_free_slot() < 0);
    idle.eid--;
    idle.window = window;
    present_buffer_idle(&idle);
    assert(present_free_slot() < 0);
    idle.window = root;
    present_buffer_idle(&idle);
    assert(present_free_slot() == 0);
    partial_frame(window, gc, 90, 0x0000ff);
    assert(submitted == first);
    assert(get_pixel(first, 55, 55) == 0xff0000);
    assert(get_pixel(first, 95, 55) == 0x00ff00);
    assert(get_pixel(first, 135, 55) == 0x0000ff);
    present_buffer_idle(&idle); /* stale Idle for previous use of same pixmap */
    assert(present_bufs[0].busy);
    puts("PASS: reuse requires matching Idle pixmap, serial and event owner");

    complete(submitted_serial);
    dirty = 1;
    last_paint = now_ms() - 17;
    long long now = now_ms();
    for (int i = 0; i < PRESENT_BUFFERS; i++) present_bufs[i].sent_ms = now;
    assert(present_wait_ms(now) == PRESENT_TIMEOUT_MS);
    assert(!repaint_due());
    for (int i = 0; i < PRESENT_BUFFERS; i++) present_bufs[i].sent_ms -= 201;
    Pixmap canonical = back_buf;
    assert(repaint_due());
    assert(!present_ok && back_buf == canonical);
    for (int i = 0; i < PRESENT_BUFFERS; i++)
        assert(present_bufs[i].pixmap == None);
    repaint();
    assert(submissions == 4);
    assert(get_pixel(root, 55, 55) == 0xff0000);
    assert(get_pixel(root, 135, 55) == 0x0000ff);
    puts("PASS: missing Idle has deadline; fallback uses separate canonical buffer");

    present_ok = 1;
    dirty = full_dirty = 1;
    repaint();
    assert(present_pending);
    dirty = fading = 0;
    present_sent_ms = now_ms() - 201;
    assert(!repaint_due());
    assert(!present_ok && !present_pending);
    puts("PASS: missing Complete times out even without damage/animation");

    present_ok = 1;
    dirty = full_dirty = 1;
    repaint();
    Pixmap retired = submitted;
    /* Servers may deliver Idle before Complete in copy mode. Idle frees
     * storage, but submission throttle must still wait for Complete. */
    idle.pixmap = submitted;
    idle.serial_number = submitted_serial;
    present_buffer_idle(&idle);
    assert(present_pending && present_free_slot() == 0);
    dirty = 1;
    last_paint = now_ms() - 17;
    assert(!repaint_due());
    complete(submitted_serial);
    assert(repaint_due());
    puts("PASS: Idle-before-Complete does not bypass pending submission gate");
    XWindowAttributes attrs;
    assert(XGetWindowAttributes(dpy, root, &attrs));
    ensure_targets(attrs.width - 1, attrs.height - 1, attrs.visual, attrs.depth);
    assert(present_free_slot() == 0 && present_bufs[0].pixmap == None);
    idle.pixmap = retired;
    idle.serial_number = submitted_serial;
    present_buffer_idle(&idle); /* resize's late Idle is harmless */
    assert(present_bufs[0].pixmap == None);
    puts("PASS: resize retires busy presentation storage without reusing it");
    present_buffers_retire();
    XFreeGC(dpy, gc);
    XFreeGC(dpy, present_gc);
    XCompositeUnredirectSubwindows(dpy, root, CompositeRedirectManual);
    XCloseDisplay(dpy);
    return 0;
}
