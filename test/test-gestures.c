/* Offline: compile with src/gestures.c src/swipe.c and Xft include flags. */
#define _POSIX_C_SOURCE 200809L
#include "../src/state.h"
#include "../src/gestures.h"
#include <X11/extensions/XInput2.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <float.h>
Display *dpy;
Window root;
Drag drag;
int SWIPE_ENABLED = 1, SWIPE_WRAP, SWIPE_REVERSE, SWIPE_DEBUG;
double SWIPE_THRESHOLD = 80;
int curws = 1, NWS = 4;
Time last_evtime;
static int calls, selected, status, clean = 1, target = -1;
static int overview_open, pending, shows, hides, selections;
int overview_is_open(void) { return overview_open; }
int overview_selection(void) { return pending; }
void overview_show(int arg) {
    (void)arg; shows++;
    if (!overview_open) { overview_open = 1; pending = curws; }
}
void overview_hide(int arg) { (void)arg; hides++; overview_open = 0; }
void overview_select(int n) { pending = n; selections++; }
void view(int n) { target = n; calls++; }
void trap_errors(Display *d) { (void)d; }
int untrap_errors(Display *d) { (void)d; return clean; }
int XISelectEvents(Display *d, Window w, XIEventMask *m, int n)
{
    (void)d; (void)w;
    assert(n == 2);
    assert(m[0].deviceid == XIAllMasterDevices);
    assert(m[1].deviceid == XIAllDevices);
    assert(!!XIMaskIsSet(m[0].mask, XI_GestureSwipeBegin) == !!SWIPE_ENABLED);
    assert(!!XIMaskIsSet(m[0].mask, XI_GestureSwipeUpdate) == !!SWIPE_ENABLED);
    assert(!!XIMaskIsSet(m[0].mask, XI_GestureSwipeEnd) == !!SWIPE_ENABLED);
    assert(!XIMaskIsSet(m[0].mask, XI_RawButtonPress));
    assert(!!XIMaskIsSet(m[1].mask, XI_HierarchyChanged) == !!SWIPE_ENABLED);
    selected++;
    return status;
}
static void event(int type, int fingers, double dx)
{
    XIGestureSwipeEvent ev = {.deviceid=2, .sourceid=7, .detail=fingers,
        .delta_x=dx, .time=123};
    XIHierarchyEvent hierarchy = {.flags=1, .num_info=0};
    XIDeviceChangedEvent changed = {.deviceid=2, .sourceid=7, .reason=1};
    XGenericEventCookie cookie = {.evtype=type, .data=&ev};
    if (type == XI_HierarchyChanged) cookie.data = &hierarchy;
    if (type == XI_DeviceChanged) cookie.data = &changed;
    gestures_event(&cookie);
}
static void gesture(int fingers)
{
    event(XI_GestureSwipeBegin, fingers, 0);
    event(XI_GestureSwipeUpdate, fingers, -100);
    event(XI_GestureSwipeEnd, fingers, 0);
}
static FILE *capture;
static int saved;
static void capture_start(void) {
    fflush(stderr); saved = dup(2); capture = tmpfile(); assert(capture);
    assert(dup2(fileno(capture), 2) >= 0);
}
static void capture_end(const char *expected) {
    char text[16384]; fflush(stderr); rewind(capture);
    size_t n = fread(text, 1, sizeof(text)-1, capture); text[n] = 0;
    assert(dup2(saved, 2) >= 0); close(saved); fclose(capture);
    if (expected) {
        if (!strstr(text, expected)) fprintf(stderr, "expected %s in: %s", expected, text);
        assert(strstr(text, expected));
    } else assert(n == 0);
    assert(n < 4096); /* No per-Update or overlapping-Begin flood. */
}
static void diagnostic(const char *reason, int fingers, double dx, double dy, int cancel) {
    capture_start();
    event(XI_GestureSwipeBegin, fingers, 0);
    XIGestureSwipeEvent ev = {.deviceid=2, .sourceid=7, .detail=fingers,
        .delta_x=dx, .delta_y=dy, .flags=cancel ? XIGestureSwipeEventCancelled : 0};
    XGenericEventCookie cookie = {.evtype=XI_GestureSwipeEnd, .data=&ev};
    gestures_event(&cookie); capture_end(reason);
}
int main(void)
{
    capture_start();
#ifdef DANIWM_NO_GESTURES
    gestures_init(2, 4); gestures_configure(); gestures_reset("drag");
    gesture(3); assert(!selected && !calls);
    capture_end(NULL);
    SWIPE_DEBUG = 1; capture_start(); gestures_init(2, 4);
    capture_end("compiled=0");
    /* Keep diagnostic helper referenced in disabled builds. */
    (void)diagnostic;
    puts("gestures disabled: ok");
    return 0;
#endif
    gestures_init(2, 3); gesture(3); assert(!selected && !calls);
    gestures_init(2, 4); assert(selected == 1);
    event(XI_GestureSwipeEnd, 3, -100); assert(!calls);
    gesture(2); assert(!calls);
    drag.win = 9; gesture(3); drag.win = None; assert(!calls);
    event(XI_GestureSwipeBegin, 3, 0);
    event(XI_GestureSwipeUpdate, 3, -100);
    assert(!calls && !last_evtime);
    gestures_reset("drag"); event(XI_GestureSwipeEnd, 3, 0); assert(!calls);
    event(XI_GestureSwipeBegin, 3, 0);
    event(XI_HierarchyChanged, 0, 0);
    event(XI_GestureSwipeEnd, 3, -100); assert(!calls);
    event(XI_GestureSwipeBegin, 3, 0);
    event(XI_DeviceChanged, 0, 0);
    event(XI_GestureSwipeEnd, 3, -100); assert(!calls);
    status = BadValue; gestures_configure(); gesture(3); assert(!calls);
    status = Success; clean = 0; gestures_configure(); gesture(3); assert(!calls);
    clean = 1; SWIPE_ENABLED = 0; gestures_configure(); gesture(3); assert(!calls);
    SWIPE_ENABLED = 1; gestures_configure(); gesture(3);
    assert(calls == 1 && target == 2 && last_evtime == 123);
    capture_end(NULL);
    SWIPE_DEBUG = 1;
    capture_start(); gestures_init(2, 3); capture_end("supported=0");
    capture_start(); gestures_init(2, 4); capture_end("active=1");
    diagnostic("reason=accepted", 3, -100, 0, 0);
    /* Horizontal swipe with minor-axis reversal must reach view(), once. */
    for (int sign = -1; sign <= 1; sign += 2) {
        curws = 1;
        int before = calls;
        capture_start(); event(XI_GestureSwipeBegin, 3, 0);
        XIGestureSwipeEvent ev = {.deviceid=2, .sourceid=7, .detail=3,
            .delta_x=sign * 50, .delta_y=10, .time=123};
        XGenericEventCookie cookie = {.evtype=XI_GestureSwipeUpdate, .data=&ev};
        gestures_event(&cookie);
        ev.delta_y = -10;
        gestures_event(&cookie);
        assert(calls == before);
        event(XI_GestureSwipeEnd, 3, 0);
        assert(calls == before + 1 && target == (sign < 0 ? 2 : 0));
        event(XI_GestureSwipeEnd, 3, 0);
        assert(calls == before + 1);
        capture_end("reason=accepted");
    }
    diagnostic("reason=below-threshold", 3, -10, 0, 0);
    diagnostic("reason=too-vertical", 3, -100, 60, 0);
    diagnostic("reason=cancelled", 3, -100, 0, 1);
    diagnostic("reason=wrong-fingers", 2, -100, 0, 0);
    curws = 0; diagnostic("clamped", 3, 100, 0, 0);
    capture_start(); event(XI_GestureSwipeBegin, 3, 0);
    event(XI_GestureSwipeUpdate, 3, 100); event(XI_GestureSwipeEnd, 3, -20);
    capture_end("reason=axis-reversal");
    capture_start(); event(XI_GestureSwipeBegin, 3, 0);
    event(XI_GestureSwipeUpdate, 3, DBL_MAX); event(XI_GestureSwipeEnd, 3, DBL_MAX);
    capture_end("reason=numeric-overflow");
    capture_start(); event(XI_GestureSwipeBegin, 3, 0);
    event(XI_GestureSwipeUpdate, 3, 100); event(XI_GestureSwipeUpdate, 3, -20);
    event(XI_GestureSwipeEnd, 3, DBL_MAX); capture_end("reason=axis-reversal");
    capture_start(); event(XI_GestureSwipeBegin, 2, 0);
    event(XI_GestureSwipeEnd, 3, DBL_MAX); capture_end("reason=wrong-fingers");
    const char *reasons[] = {"config-reload", "drag", "hierarchy-change", "device-change"};
    for (int i=0; i<4; i++) {
        capture_start(); event(XI_GestureSwipeBegin, 3, 0);
        if (!i) gestures_configure();
        else if (i==1) gestures_reset("drag");
        else event(i==2 ? XI_HierarchyChanged : XI_DeviceChanged, 0, 0);
        event(XI_GestureSwipeEnd, 3, -100); capture_end(reasons[i]);
        capture_start(); event(XI_GestureSwipeBegin, 3, 0);
        gestures_reset("drag"); capture_end("device=2 source=7");
    }
    capture_start(); event(XI_GestureSwipeBegin, 3, 0);
    for (int i=0; i<1000; i++) {
        event(XI_GestureSwipeBegin, 3, 0);
        event(XI_GestureSwipeUpdate, 3, 1);
    }
    event(XI_GestureSwipeEnd, 3, -100); capture_end("reason=overlapping-begin");
    /* Overview routing remains End-only and never commits pending selection. */
    int before = calls;
    Time time_before = last_evtime;
    curws = 1;
    for (int repeat = 0; repeat < 2; ++repeat) {
        capture_start();
        event(XI_GestureSwipeBegin, 3, 0);
        XIGestureSwipeEvent ev = {.deviceid=2, .sourceid=7, .detail=3,
            .delta_y=-100, .time=456};
        XGenericEventCookie cookie = {.evtype=XI_GestureSwipeUpdate, .data=&ev};
        int old_shows = shows;
        gestures_event(&cookie);
        assert(shows == old_shows);
        event(XI_GestureSwipeEnd, 3, 0);
        assert(shows == old_shows + 1 && overview_open && pending == 1);
        event(XI_GestureSwipeEnd, 3, 0);
        assert(shows == old_shows + 1);
        capture_end("overview-show");
    }
    capture_start(); gesture(3); capture_end("overview-select");
    assert(pending == 2 && selections == 1 && curws == 1);
    capture_start(); gesture(3); capture_end("overview-select");
    assert(pending == 3);
    SWIPE_WRAP = 1;
    capture_start(); gesture(3); capture_end("overview-select");
    assert(pending == 0);
    diagnostic("overview-select", 3, 100, 0, 0);
    assert(pending == 3);
    SWIPE_REVERSE = 1;
    diagnostic("overview-select", 3, -100, 0, 0);
    assert(pending == 2);
    SWIPE_WRAP = SWIPE_REVERSE = 0;
    int actions = shows + hides + selections;
    diagnostic("reason=cancelled", 3, 0, -100, 1);
    diagnostic("reason=wrong-fingers", 2, 0, -100, 0);
    diagnostic("reason=below-threshold", 3, 0, -10, 0);
    diagnostic("reason=cancelled", 3, 0, 100, 1);
    diagnostic("reason=wrong-fingers", 2, 0, 100, 0);
    diagnostic("reason=below-threshold", 3, 0, 10, 0);
    assert(actions == shows + hides + selections && pending == 2);
    for (int repeat = 0; repeat < 2; ++repeat) {
        diagnostic("overview-hide", 3, 0, 100, 0);
        assert(!overview_open && curws == 1);
    }
    assert(shows == 2 && hides == 2 && calls == before && last_evtime == time_before);
    puts("gestures: ok");
}
