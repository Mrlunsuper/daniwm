/* float-click-helper.c - tiny X app logging press AND release.
 * class: "FloatClick" — usable both tiled and floating in tests. */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    Display *d = XOpenDisplay(NULL);
    if (!d) { fprintf(stderr, "float-click-helper: cannot open display\n"); return 1; }
    int s = DefaultScreen(d);
    Window root = RootWindow(d, s);
    Window w = XCreateSimpleWindow(d, root, 0, 0, 300, 200, 1,
        BlackPixel(d, s), WhitePixel(d, s));
    XClassHint ch = { .res_name = "floatclick", .res_class = "FloatClick" };
    XSetClassHint(d, w, &ch);
    XStoreName(d, w, "float-click-helper");
    XSelectInput(d, w, ButtonPressMask | ButtonReleaseMask | ExposureMask | StructureNotifyMask);
    XMapWindow(d, w);
    XFlush(d);
    printf("READY %lu\n", (unsigned long)w);
    fflush(stdout);
    for (;;) {
        XEvent ev;
        XNextEvent(d, &ev);
        if (ev.type == ButtonPress)
            printf("PRESS button=%u state=%u\n", ev.xbutton.button, ev.xbutton.state);
        else if (ev.type == ButtonRelease)
            printf("RELEASE button=%u\n", ev.xbutton.button);
        fflush(stdout);
    }
    return 0;
}