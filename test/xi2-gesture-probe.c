/* Standalone diagnostic only: never linked into the WM. */
#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(XI_GestureSwipeEnd) && defined(XIGestureClass) && !defined(PROBE_NO_GESTURES)
#define HAVE_GESTURES 1
#endif

#ifdef HAVE_GESTURES
static volatile sig_atomic_t stopped;
static int errors;
static void stop(int sig) { (void)sig; stopped = 1; }
static int xerror(Display *d, XErrorEvent *e)
{
    char text[256];
    XGetErrorText(d, e->error_code, text, sizeof text);
    fprintf(stderr, "X error: %s request=%u minor=%u resource=%lu\n",
            text, e->request_code, e->minor_code, e->resourceid);
    errors++;
    return 0;
}
static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}
static void swipe(const XIGestureSwipeEvent *e)
{
    const char *phase = e->evtype == XI_GestureSwipeBegin ? "begin" :
                        e->evtype == XI_GestureSwipeUpdate ? "update" : "end";
    printf("swipe %s time=%lu device=%d source=%d detail=%d "
           "dx=%.6f dy=%.6f unaccel_dx=%.6f unaccel_dy=%.6f "
           "flags=0x%x cancelled=%d root=%lu event=%lu child=%lu mods=0x%x\n",
           phase, e->time, e->deviceid, e->sourceid, e->detail,
           e->delta_x, e->delta_y, e->delta_unaccel_x, e->delta_unaccel_y,
           e->flags, !!(e->flags & XIGestureSwipeEventCancelled),
           e->root, e->event, e->child, e->mods.effective);
}
static int selftest(void)
{
    unsigned char mask[XIMaskLen(XI_GestureSwipeEnd)] = {0};
    XISetMask(mask, XI_GestureSwipeEnd);
    if (sizeof mask < 5 || !XIMaskIsSet(mask, XI_GestureSwipeEnd)) return 1;
    printf("mask bytes=%zu end=%d covered=1\n", sizeof mask, XI_GestureSwipeEnd);
    XIGestureSwipeEvent e = {0};
    e.evtype = XI_GestureSwipeBegin; e.detail = 3; swipe(&e);
    e.evtype = XI_GestureSwipeUpdate; e.delta_x = -1.5; e.delta_y = 2.5; swipe(&e);
    e.evtype = XI_GestureSwipeEnd; e.flags = XIGestureSwipeEventCancelled; swipe(&e);
    puts("self-test: PASS (synthetic decoding only)");
    return 0;
}
#endif
static int usage(void)
{
    fputs("usage: xi2-gesture-probe --self-test | --build-info |\n"
          "  --display NAME --mode capabilities|observe|grab [--seconds 1..60]\n"
          "  grab additionally requires --allow-passive-grab\n"
          "No inherited DISPLAY fallback. Observe/grab require explicit prior approval.\n", stderr);
    return 2;
}
int main(int argc, char **argv)
{
    const char *display = NULL, *mode = NULL;
    int seconds = 10, allow = 0, duration_set = 0;
    if (argc == 2 && !strcmp(argv[1], "--build-info")) {
#ifdef HAVE_GESTURES
        puts("build: XI2.4 gesture API supported");
#else
        puts("unsupported: build lacks XI2.4 gesture API");
#endif
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--self-test")) {
#ifdef HAVE_GESTURES
        return selftest();
#else
        puts("unsupported: build lacks XI2.4 gesture API");
        return 3;
#endif
    }
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--display") && i + 1 < argc && !display) display = argv[++i];
        else if (!strcmp(argv[i], "--mode") && i + 1 < argc && !mode) mode = argv[++i];
        else if (!strcmp(argv[i], "--allow-passive-grab") && !allow) allow = 1;
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc && !duration_set) {
            char *end;
            errno = 0;
            long n = strtol(argv[++i], &end, 10);
            if (errno || !*argv[i] || *end || n < 1 || n > 60) return usage();
            seconds = (int)n; duration_set = 1;
        } else return usage();
    }
    if (!display || !*display || !mode ||
        (strcmp(mode, "capabilities") && strcmp(mode, "observe") && strcmp(mode, "grab")) ||
        ((strcmp(mode, "grab") == 0) != allow) ||
        (!strcmp(mode, "capabilities") && duration_set)) return usage();
#ifndef HAVE_GESTURES
    (void)seconds;
    puts("unsupported: build lacks XI2.4 gesture API; no connection opened");
    return 3;
#else
    Display *d = XOpenDisplay(display);
    if (!d) { fprintf(stderr, "cannot open explicit display: %s\n", display); return 1; }
    XSetErrorHandler(xerror);
    int opcode, event, error, major = 2, minor = 4, rc = 0;
    if (!XQueryExtension(d, "XInputExtension", &opcode, &event, &error)) {
        puts("unsupported: no XInput extension"); rc = 3; goto close;
    }
    int version_status = XIQueryVersion(d, &major, &minor);
    XSync(d, False);
    printf("XI negotiated=%d.%d status=%d\n", major, minor, version_status);
    if (version_status != Success || errors || major < 2 || (major == 2 && minor < 4)) {
        puts("unsupported: XI2.4 required"); rc = 3; goto close;
    }
    int count = 0;
    XIDeviceInfo *devices = XIQueryDevice(d, XIAllDevices, &count);
    XSync(d, False);
    if (!devices || errors) { if (devices) XIFreeDeviceInfo(devices); rc = 1; goto close; }
    for (int i = 0; i < count; i++) {
        XIDeviceInfo *dev = &devices[i];
        printf("device=%d use=%d attachment=%d enabled=%d name=%s\n",
               dev->deviceid, dev->use, dev->attachment, dev->enabled, dev->name);
        for (int j = 0; j < dev->num_classes; j++) {
            XIAnyClassInfo *c = dev->classes[j];
            printf(" class=%d source=%d", c->type, c->sourceid);
            if (c->type == XIGestureClass)
                printf(" gesture-max-touches=%d", ((XIGestureClassInfo *)c)->num_touches);
            putchar('\n');
        }
    }
    puts("Capabilities are not evidence of physical gesture delivery.");
    fflush(stdout);
    if (!strcmp(mode, "capabilities")) { XIFreeDeviceInfo(devices); goto close; }
    unsigned char bits[XIMaskLen(XI_GestureSwipeEnd)] = {0};
    XISetMask(bits, XI_GestureSwipeBegin);
    XISetMask(bits, XI_GestureSwipeUpdate);
    XISetMask(bits, XI_GestureSwipeEnd);
    XIEventMask mask = {XIAllMasterDevices, sizeof bits, bits};
    Window root = DefaultRootWindow(d);
    int grab = !strcmp(mode, "grab");
    if (!grab) {
        int status = XISelectEvents(d, root, &mask, 1);
        printf("select root=%lu status=%d\n", root, status);
        if (status != Success) rc = 1;
    } else {
        int masters = 0;
        puts("WARNING: passive grab has NO finger-count filter; all swipe counts may be intercepted.");
        for (int i = 0; i < count; i++) {
            if (devices[i].use != XIMasterPointer || !devices[i].enabled) continue;
            XIGrabModifiers mod = {XIAnyModifier, 0};
            masters++;
            mask.deviceid = devices[i].deviceid;
            int status = XIGrabSwipeGestureBegin(d, mask.deviceid, root,
                         XIGrabModeAsync, XIGrabModeAsync, False, &mask, 1, &mod);
            XSync(d, False);
            printf("grab root=%lu master=%d modifiers=0x%x request-status=%d modifier-status=%d\n",
                   root, mask.deviceid, mod.modifiers, status, mod.status);
            if (status != Success || mod.status != XIGrabSuccess) rc = 1;
        }
        if (!masters) { fputs("no enabled master pointer to probe\n", stderr); rc = 1; }
    }
    XSync(d, False);
    if (errors) rc = 1;
    signal(SIGINT, stop); signal(SIGTERM, stop);
    double deadline = now() + seconds;
    printf("bounded observation seconds=%d mode=%s\n", seconds, mode); fflush(stdout);
    while (!rc && !stopped && now() < deadline) {
        /* Bound even a continuously busy event stream. */
        if (XPending(d)) {
            XEvent e;
            XNextEvent(d, &e);
            if (e.type == GenericEvent && e.xcookie.extension == opcode &&
                e.xcookie.evtype >= XI_GestureSwipeBegin && e.xcookie.evtype <= XI_GestureSwipeEnd &&
                XGetEventData(d, &e.xcookie)) {
                if (e.xcookie.data) swipe(e.xcookie.data);
                XFreeEventData(d, &e.xcookie);
                fflush(stdout);
            }
        } else {
            struct pollfd p = {ConnectionNumber(d), POLLIN, 0};
            int n = poll(&p, 1, 100);
            if ((n < 0 && errno != EINTR) || (n > 0 && (p.revents & (POLLERR | POLLHUP | POLLNVAL)))) rc = 1;
        }
    }
    if (grab) {
        for (int i = 0; i < count; i++) {
            if (devices[i].use != XIMasterPointer || !devices[i].enabled) continue;
            XIGrabModifiers mod = {XIAnyModifier, 0};
            int status = XIUngrabSwipeGestureBegin(d, devices[i].deviceid, root, 1, &mod);
            printf("ungrab master=%d status=%d\n", devices[i].deviceid, status);
            if (status != Success) rc = 1;
        }
    } else {
        memset(bits, 0, sizeof bits);
        XISelectEvents(d, root, &mask, 1);
    }
    XSync(d, False);
    XIFreeDeviceInfo(devices);
    puts("cleanup: closing probe connection (releases all probe-owned grabs)");
close:
    XCloseDisplay(d);
    return errors ? 1 : rc;
#endif
}
