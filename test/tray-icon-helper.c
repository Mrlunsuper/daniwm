/* tray-icon-helper: minimal XEmbed tray icon for headless tests.
 * Creates a 24x24 window with _XEMBED_INFO, sends SYSTEM_TRAY_REQUEST_DOCK
 * to the _NET_SYSTEM_TRAY_S<n> owner, maps, then waits for kill.
 * Prints "DOCKED parent=<id>" so the test can assert reparenting. */
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

static volatile int running = 1;
static void onterm(int s) { (void)s; running = 0; }

int main(void) {
    signal(SIGTERM, onterm);
    signal(SIGINT, onterm);
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "no display\n"); return 1; }
    int screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, screen);

    Window win = XCreateSimpleWindow(dpy, root, 0, 0, 24, 24, 0, 0, 0xff0000);

    Atom xembed_info = XInternAtom(dpy, "_XEMBED_INFO", False);
    long info[2] = { 0, 1 }; /* version 0, MAPPED */
    XChangeProperty(dpy, win, xembed_info, xembed_info, 32,
        PropModeReplace, (unsigned char *)info, 2);

    char selname[32];
    snprintf(selname, sizeof(selname), "_NET_SYSTEM_TRAY_S%d", screen);
    Atom sel = XInternAtom(dpy, selname, False);
    Atom opcode = XInternAtom(dpy, "_NET_SYSTEM_TRAY_OPCODE", False);

    /* wait up to 5s for the tray manager */
    Window owner = None;
    for (int i = 0; i < 50; i++) {
        owner = XGetSelectionOwner(dpy, sel);
        if (owner != None) break;
        usleep(100000);
    }
    if (owner == None) { fprintf(stderr, "no tray owner\n"); return 2; }

    XEvent ev;
    ev.xclient.type = ClientMessage;
    ev.xclient.window = owner;
    ev.xclient.message_type = opcode;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = CurrentTime;
    ev.xclient.data.l[1] = 0; /* REQUEST_DOCK */
    ev.xclient.data.l[2] = (long)win;
    ev.xclient.data.l[3] = 0;
    ev.xclient.data.l[4] = 0;
    XSendEvent(dpy, owner, False, NoEventMask, &ev);
    XFlush(dpy);

    XMapWindow(dpy, win);
    XFlush(dpy);

    /* wait for reparent (up to 3s), then report */
    for (int i = 0; i < 30; i++) {
        Window r, p, *kids = NULL;
        unsigned nk = 0;
        if (XQueryTree(dpy, win, &r, &p, &kids, &nk)) {
            if (kids) XFree(kids);
            if (p != root) break;
        }
        usleep(100000);
    }
    {
        Window r, p, *kids = NULL;
        unsigned nk = 0;
        if (XQueryTree(dpy, win, &r, &p, &kids, &nk)) {
            if (kids) XFree(kids);
            printf("DOCKED parent=0x%lx root=0x%lx win=0x%lx\n", p, root, win);
            fflush(stdout);
        }
    }

    while (running) {
        while (XPending(dpy)) { XNextEvent(dpy, &ev); }
        usleep(50000);
    }
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return 0;
}
