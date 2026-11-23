/* Phase 1A: real renderer/settings tests on the wrapper's private X server. */
#define main comp_main
#include "../src/comp.c"
#undef main
#include <X11/Xutil.h>
#include <assert.h>

static void publish(Window owner, unsigned long enabled, unsigned long reduced, unsigned long ms) {
    unsigned long words[] = {1, owner, enabled, reduced, ms};
    XChangeProperty(dpy, root, A_ANIMATION_SETTINGS, XA_CARDINAL, 32,
        PropModeReplace, (unsigned char *)words, 5);
    animation_settings_read();
}
static unsigned long pixel(Drawable drawable, int x, int y) {
    XImage *image = XGetImage(dpy, drawable, x, y, 1, 1, AllPlanes, ZPixmap);
    assert(image);
    unsigned long p = XGetPixel(image, 0, 0);
    XDestroyImage(image);
    return p;
}
int main(int argc, char **argv) {
    assert(argc == 2 && argv[1][0] == ':');
    dpy = XOpenDisplay(argv[1]); assert(dpy);
    screen = DefaultScreen(dpy); root = RootWindow(dpy, screen);
    int event;
    assert(XDamageQueryExtension(dpy, &dmg_event, &dmg_error));
    assert(XRenderQueryExtension(dpy, &event, &render_errbase));
    shape_ok = XShapeQueryExtension(dpy, &shape_event, &event);
    XSetErrorHandler(xerror_ignore);
    XCompositeRedirectSubwindows(dpy, root, CompositeRedirectManual);
    A_ANIMATION_SETTINGS = XInternAtom(dpy, ANIMATION_SETTINGS_ATOM, False);
    A_WM_CHECK = XInternAtom(dpy, "_NET_SUPPORTING_WM_CHECK", False);
    A_OPACITY = XInternAtom(dpy, "_NET_WM_WINDOW_OPACITY", False);
    Window wm = XCreateSimpleWindow(dpy, root, 0, 0, 1, 1, 0, 0, 0);
    XChangeProperty(dpy, wm, A_WM_CHECK, XA_WINDOW, 32, PropModeReplace, (unsigned char *)&wm, 1);
    XChangeProperty(dpy, root, A_WM_CHECK, XA_WINDOW, 32, PropModeReplace, (unsigned char *)&wm, 1);
    opt_shadow = 0; opt_dim = 1;
    opt_wm_settings = 1;
    animation_settings_read(); assert(!opt_fade);
    publish(wm, 1, 0, 1000); assert(opt_fade && opt_fade_ms == 1000);
    opt_force_reduce_motion = 1;
    publish(wm, 1, 0, 1000); assert(opt_reduce_motion);
    opt_force_reduce_motion = 0;
    publish(wm, 1, 0, 1000); assert(!opt_reduce_motion);
    Window window = XCreateSimpleWindow(dpy, root, 40, 40, 200, 120, 0, 0, 0xffffff);
    XMapWindow(dpy, window); XSync(dpy, False);
    Win *w = win_add(window); assert(w && w->opening.active);
    long long now = now_ms();
    opening_start(w, now - 500);
    double alpha = win_alpha(w, now);
    assert(alpha == 0.5 && fading);
    repaint();
    unsigned long p = pixel(back_buf, 100, 100);
    assert((p & 255) > 100 && (p & 255) < 180);
    /* A content-only damage box must not restrict the next fade frame. */
    dmg_add(40, 40, 45, 45); dirty = 1;
    repaint(); assert(!cur_clip_on);
    publish(wm, 1, 1, 1000);
    assert(!w->opening.active && !fading && full_dirty);
    repaint(); assert(pixel(back_buf, 100, 100) == 0xffffff);
    assert(!fading && !repaint_due());
    unsigned long opacity = OPAQUE / 2;
    XChangeProperty(dpy, window, A_OPACITY, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&opacity, 1);
    win_refresh_flags(w);
    assert(w->opacity == opacity);
    publish(wm, 1, 0, 1000);
    assert(!w->opening.active); /* enabling never restarts an existing window */
    opening_start(w, now);
    assert(win_alpha(w, now + 500) > 0.249 && win_alpha(w, now + 500) < 0.251);
    assert(!win_is_opaque(w, now + 500));
    publish(wm, 0, 0, 1000); assert(!w->opening.active && !fading);
    publish(wm, 1, 0, 0); opening_start(w, now);
    assert(!w->opening.active && !fading);
    /* Short/oversized/out-of-range, foreign-owner and deleted settings fail instant. */
    unsigned long bad[] = {1, wm, 1, 0, 1001, 0};
    for (unsigned n = 4; n <= 6; n++) {
        XChangeProperty(dpy, root, A_ANIMATION_SETTINGS, XA_CARDINAL, 32, PropModeReplace,
                        (unsigned char *)bad, (int)n);
        animation_settings_read(); assert(!opt_fade);
    }
    opt_wm_settings = 0; opt_fade = 1; opt_fade_ms = 160;
    publish(wm, 0, 1, 0);
    assert(opt_fade && !opt_reduce_motion && opt_fade_ms == 160);
    opt_wm_settings = 1;
    publish(wm + 1, 1, 0, 140); assert(!opt_fade);
    publish(wm, 1, 0, 140); assert(opt_fade);
    XChangeProperty(dpy, root, A_ANIMATION_SETTINGS, XA_STRING, 8, PropModeReplace,
                    (unsigned char *)"bad", 3);
    animation_settings_read(); assert(!opt_fade);
    XDeleteProperty(dpy, root, A_ANIMATION_SETTINGS);
    animation_settings_read(); assert(!opt_fade);
    publish(wm, 1, 0, 140);
    XDestroyWindow(dpy, wm); XSync(dpy, False);
    animation_settings_read(); assert(!opt_fade && !w->opening.active);
    /* Standalone ignores even valid/malicious WM settings unless opted in. */
    opt_wm_settings = 0; opt_fade = 1; opt_fade_ms = 160; opt_reduce_motion = 0;
    animation_settings_read(); assert(opt_fade && opt_fade_ms == 160);
    w->opacity = OPAQUE;
    w->override = 1; opening_start(w, now); assert(!w->opening.active);
    w->override = 0; w->dock = 1; opening_start(w, now); assert(!w->opening.active);
    w->dock = 0; w->fullscreen = 1; opening_start(w, now); assert(!w->opening.active);
    w->fullscreen = 0;
    opt_reduce_motion = 1;
    opening_start(w, now); assert(!w->opening.active);
    opt_reduce_motion = 0;
    opening_start(w, now); assert(w->opening.active);
    uint64_t generation = w->generation;
    win_del(window);
    w = win_add(window); assert(w && w->generation != generation);
    /* Real ARGB pixels must retain their alpha when fade is applied. */
    XVisualInfo info;
    assert(XMatchVisualInfo(dpy, screen, 32, TrueColor, &info));
    XSetWindowAttributes a = {0};
    a.colormap = XCreateColormap(dpy, root, info.visual, AllocNone);
    a.background_pixel = 0x80402010; a.border_pixel = 0;
    Window argb = XCreateWindow(dpy, root, 300, 40, 32, 32, 0, 32, InputOutput,
        info.visual, CWColormap | CWBackPixel | CWBorderPixel, &a);
    XMapWindow(dpy, argb); XSync(dpy, False);
    Win *aw = win_add(argb); assert(aw && aw->depth == 32);
    opening_start(aw, now);
    assert(!win_is_opaque(aw, now + 80));
    XRectangle rect = {0, 0, 10, 10};
    XShapeCombineRectangles(dpy, window, ShapeBounding, 0, 0, &rect, 1, ShapeSet, Unsorted);
    assert(ensure_win_pict(w)); assert(w->shaped && !win_is_opaque(w, now + 160));
    animation_cancel(&w->opening);
    opening_start(aw, now_ms() - 80);
    dirty = full_dirty = stack_dirty = 1;
    repaint();
    unsigned long half_argb = pixel(back_buf, 310, 50);
    assert(((half_argb >> 16) & 255) >= 28 && ((half_argb >> 16) & 255) <= 40);
    assert(pixel(back_buf, 100, 100) == 0); /* shaped-out area, no black/opaque overlay */
    animation_cancel(&aw->opening);
    fading = 0; dirty = full_dirty = 1;
    repaint();
    assert(pixel(back_buf, 310, 50) == 0x402010);
    win_del(argb); win_del(window);
    XFreeColormap(dpy, a.colormap);
    XCompositeUnredirectSubwindows(dpy, root, CompositeRedirectManual);
    XCloseDisplay(dpy);
    puts("PASS: Phase 1A settings validation, standalone isolation, pixel fade/cancel, opacity, eligibility, generations and shape/ARGB");
    return 0;
}
