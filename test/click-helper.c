/* click-helper.c - tiny X app for click-delivery tests.
 * Maps a normal window, selects ButtonPressMask on its OWN window (always
 * allowed), and logs every press it receives to stdout:
 *   READY <winid>
 *   PRESS button=<b> state=<s>
 * Used to prove the WM replays grabbed clicks back to the app. */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    Display *d = XOpenDisplay(NULL);
    if (!d) { fprintf(stderr, "click-helper: cannot open display\n"); return 1; }
    int s = DefaultScreen(d);
    Window root = RootWindow(d, s);
    Window w = XCreateSimpleWindow(d, root, 0, 0, 300, 200, 1,
        BlackPixel(d, s), WhitePixel(d, s));
    XClassHint ch = { .res_name = "clickhelper", .res_class = "ClickHelper" };
    XSetClassHint(d, w, &ch);
    XStoreName(d, w, "click-helper");
    XSelectInput(d, w, ButtonPressMask | ExposureMask | StructureNotifyMask);
    XMapWindow(d, w);
    XFlush(d);
    printf("READY %lu\n", (unsigned long)w);
    fflush(stdout);
    for (;;) {
        XEvent ev;
        XNextEvent(d, &ev);
        if (ev.type == ButtonPress) {
            printf("PRESS button=%u state=%u\n",
                ev.xbutton.button, ev.xbutton.state);
            fflush(stdout);
        }
    }
    return 0;
}
