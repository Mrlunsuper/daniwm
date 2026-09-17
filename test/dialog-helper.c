/* dialog-helper.c - normal (managed) window that maps and waits, like an
 * app dialog opening while a context menu is still up. */
#include <X11/Xlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv) {
    int ms = argc > 1 ? atoi(argv[1]) : 4000;
    Display *d = XOpenDisplay(NULL);
    if (!d) return 1;
    Window w = XCreateSimpleWindow(d, DefaultRootWindow(d), 100, 100, 400, 300, 0,
        BlackPixel(d, DefaultScreen(d)), WhitePixel(d, DefaultScreen(d)));
    XStoreName(d, w, "dialog-helper");
    XMapWindow(d, w);
    XSync(d, False);
    printf("DIALOG 0x%lx\n", (unsigned long)w);
    fflush(stdout);
    usleep((useconds_t)ms * 1000);
    XCloseDisplay(d);
    return 0;
}
