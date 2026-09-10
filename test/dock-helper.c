#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

static volatile int running = 1;
static void sighandler(int sig) { (void)sig; running = 0; }

int main(int argc, char **argv) {
    if (argc < 7) {
        fprintf(stderr, "Usage: %s <top> <bottom> <left> <right> <w> <h>\n", argv[0]);
        return 1;
    }
    unsigned long top = strtoul(argv[1], NULL, 10);
    unsigned long bottom = strtoul(argv[2], NULL, 10);
    unsigned long left = strtoul(argv[3], NULL, 10);
    unsigned long right = strtoul(argv[4], NULL, 10);
    int w = atoi(argv[5]);
    int h = atoi(argv[6]);

    signal(SIGTERM, sighandler);
    signal(SIGINT, sighandler);

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "Cannot open display\n");
        return 1;
    }
    int screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, screen);
    int sw = DisplayWidth(dpy, screen);
    int sh = DisplayHeight(dpy, screen);

    Window win = XCreateSimpleWindow(dpy, root, 0, 0,
                                     w > 0 ? (unsigned)w : (unsigned)sw,
                                     h > 0 ? (unsigned)h : (unsigned)top,
                                     0, 0, 0x112233);

    Atom net_wm_window_type = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    Atom net_wm_window_type_dock = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
    XChangeProperty(dpy, win, net_wm_window_type, XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)&net_wm_window_type_dock, 1);

    Atom net_wm_strut_partial = XInternAtom(dpy, "_NET_WM_STRUT_PARTIAL", False);
    unsigned long strut[12] = {
        left, right, top, bottom,
        0, (unsigned long)(sh - 1),
        0, (unsigned long)(sh - 1),
        0, (unsigned long)(sw - 1),
        0, (unsigned long)(sw - 1)
    };
    XChangeProperty(dpy, win, net_wm_strut_partial, XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)strut, 12);

    XSelectInput(dpy, win, StructureNotifyMask);
    XMapWindow(dpy, win);
    XFlush(dpy);

    while (running) {
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
        }
        usleep(50000);
    }

    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return 0;
}
