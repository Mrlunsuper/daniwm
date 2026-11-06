/* Two overlapping floats must not fight for focus under a stationary pointer.
 * Used by test-overlap-focus.sh in both tile and monocle layouts. */
#define _POSIX_C_SOURCE 200809L
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <time.h>

static Display *d;
static Window root;
static Atom active;

static long long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000LL + t.tv_nsec / 1000000;
}
static void pause_ms(int n) {
    struct timespec t = { n / 1000, (n % 1000) * 1000000L };
    nanosleep(&t, NULL);
}
static Window active_window(void) {
    Atom type;
    int format;
    unsigned long n, remaining;
    unsigned char *data = NULL;
    Window w = None;
    if (XGetWindowProperty(d, root, active, 0, 1, False, AnyPropertyType,
                          &type, &format, &n, &remaining, &data) == Success &&
        data && format == 32 && n == 1)
        w = *(Window *)data;
    if (data) XFree(data);
    return w;
}
static int focused(Window expected) {
    Window actual;
    int revert;
    XGetInputFocus(d, &actual, &revert);
    return actual == expected && active_window() == expected;
}
static Window pointer_window(void) {
    Window r, child;
    int rx, ry, wx, wy;
    unsigned mask;
    if (!XQueryPointer(d, root, &r, &child, &rx, &ry, &wx, &wy, &mask)) return None;
    return child;
}
static int stable(Window expected, const char *message) {
    /* Allow asynchronous Enter/FocusIn processing before checking stability. */
    long long deadline = now_ms() + 1000;
    while (!focused(expected) && now_ms() < deadline) pause_ms(5);
    if (!focused(expected)) {
        fprintf(stderr, "FAIL: %s: expected window never received focus\n", message);
        return 0;
    }
    deadline = now_ms() + 700;
    while (now_ms() < deadline) {
        if (!focused(expected) || pointer_window() != expected) {
            fprintf(stderr, "FAIL: %s: focus or stacking changed with pointer stationary\n", message);
            return 0;
        }
        pause_ms(5);
    }
    printf("PASS: %s\n", message);
    return 1;
}
static int hover(int x, int y, Window expected, const char *message) {
    XWarpPointer(d, None, root, 0, 0, 0, 0, x, y);
    XFlush(d);
    return stable(expected, message);
}
static Window make_window(int x, int y, unsigned w, unsigned h, const char *name) {
    Window win = XCreateSimpleWindow(d, root, x, y, w, h, 0, 0, 0x334455);
    XClassHint cls = { "overlap-focus", "OverlapFocus" };
    XSetClassHint(d, win, &cls);
    XStoreName(d, win, name);
    XMapWindow(d, win);
    XFlush(d);
    long long deadline = now_ms() + 2000;
    XWindowAttributes attr;
    do {
        XGetWindowAttributes(d, win, &attr);
        if (attr.map_state == IsViewable) break;
        pause_ms(5);
    } while (now_ms() < deadline);
    return win;
}
int main(void) {
    d = XOpenDisplay(NULL);
    if (!d) { fprintf(stderr, "FAIL: cannot open test display\n"); return 1; }
    root = DefaultRootWindow(d);
    active = XInternAtom(d, "_NET_ACTIVE_WINDOW", False);
    XWarpPointer(d, None, root, 0, 0, 0, 0, 1100, 700);
    Window a = make_window(100, 100, 600, 500, "Overlap A");
    Window b = make_window(300, 250, 500, 400, "Overlap B");
    XMoveResizeWindow(d, a, 100, 100, 600, 500);
    XMoveResizeWindow(d, b, 300, 250, 500, 400);
    XRaiseWindow(d, b);
    XSync(d, False);
    pause_ms(200);

    int ok = hover(150, 150, a, "hover exposed back float stays stable");
    if (ok) ok = hover(400, 350, b, "hover overlap keeps front float focused");
    if (ok) {
        /* Even an unchanged floating ConfigureRequest schedules arrange(). */
        XMoveWindow(d, a, 100, 100);
        XFlush(d);
        ok = stable(b, "relayout preserves floating focus and stacking");
    }
    if (ok) ok = hover(150, 150, a, "hover can return to exposed back float");
    XDestroyWindow(d, a);
    XDestroyWindow(d, b);
    XCloseDisplay(d);
    return ok ? 0 : 1;
}
