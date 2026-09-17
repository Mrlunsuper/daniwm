/* menu-helper.c - override-redirect popup that grabs the pointer, like an
 * app context menu. Maps at the pointer, holds the grab for <ms>, then
 * ungrabs and unmaps. Used to reproduce focus theft on grab release. */
#include <X11/Xlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv) {
    int ms = argc > 1 ? atoi(argv[1]) : 1000;
    Display *d = XOpenDisplay(NULL);
    if (!d) return 1;
    Window r = DefaultRootWindow(d);
    Window rr, cr; int rx, ry, wx, wy; unsigned mask;
    XQueryPointer(d, r, &rr, &cr, &rx, &ry, &wx, &wy, &mask);
    XSetWindowAttributes wa;
    wa.override_redirect = True;
    wa.background_pixel = BlackPixel(d, DefaultScreen(d));
    Window w = XCreateWindow(d, r, rx - 60, ry - 40, 200, 150, 0,
        CopyFromParent, InputOutput, CopyFromParent,
        CWOverrideRedirect | CWBackPixel, &wa);
    XMapRaised(d, w);
    XSync(d, False);
    XGrabPointer(d, w, True, ButtonPressMask | ButtonReleaseMask,
        GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
    XSync(d, False);
    printf("MENU 0x%lx\n", (unsigned long)w);
    fflush(stdout);
    usleep((useconds_t)ms * 1000);
    XUngrabPointer(d, CurrentTime);
    XUnmapWindow(d, w);
    XDestroyWindow(d, w);
    XSync(d, False);
    printf("CLOSED\n");
    fflush(stdout);
    XCloseDisplay(d);
    return 0;
}
