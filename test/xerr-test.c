/* xerr-test.c - T-H3A unit: bogus XID inside trap logs and continues. */
#include <stdio.h>
#include <X11/Xlib.h>

#include "../src/xerr.h"

static int ignore_it(Display *d, XErrorEvent *e) { (void)d; (void)e; return 0; }

int main(void) {
    Display *d = XOpenDisplay(NULL);
    if (!d) { fprintf(stderr, "no display\n"); return 2; }
    XSetErrorHandler(ignore_it);
    trap_errors(d);
    /* Bogus client id: must raise BadWindow, trapped and logged. */
    XKillClient(d, (Window)0x00FFFFFFUL);
    untrap_errors(d);
    printf("ALIVE=1\n");
    XCloseDisplay(d);
    return 0;
}
