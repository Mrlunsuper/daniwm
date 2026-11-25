#include "gestures.h"
#include "state.h"
#include <stdio.h>
#ifndef DANIWM_NO_GESTURES
#include <X11/extensions/XInput2.h>
#endif

#if !defined(DANIWM_NO_GESTURES) && defined(XI_GestureSwipeBegin) && defined(XIGestureSwipeEventCancelled)
#include "state.h"
#include "client.h"
#include "swipe.h"
#include "overview.h"
#include "xerr.h"

static Swipe swipe;
static int supported, active, xi_major, xi_minor;

void gestures_reset(const char *reason) {
    if (SWIPE_DEBUG && swipe.active)
        fprintf(stderr, "daniwm: swipe reset device=%d source=%d reason=%s dx=%g dy=%g\n",
                swipe.device, swipe.source, reason, swipe.total_dx, swipe.total_dy);
    swipe_reset(&swipe);
}

void gestures_configure(void)
{
    unsigned char masters[XIMaskLen(XI_LASTEVENT)] = {0};
    unsigned char devices[XIMaskLen(XI_LASTEVENT)] = {0};
    XIEventMask masks[] = {
        {XIAllMasterDevices, sizeof masters, masters},
        {XIAllDevices, sizeof devices, devices}
    };
    XIGrabModifiers mods = {XIAnyModifier, 0};
    int status, clean, grab = -1;
    gestures_reset("config-reload");
    active = 0;
    if (!supported) {
        if (SWIPE_DEBUG) fprintf(stderr, "daniwm: swipe configure XI=%d.%d supported=0 enabled=%d active=0 status=not-attempted trap=not-attempted\n", xi_major, xi_minor, SWIPE_ENABLED);
        return;
    }
    if (SWIPE_ENABLED) {
        XISetMask(masters, XI_GestureSwipeBegin);
        XISetMask(masters, XI_GestureSwipeUpdate);
        XISetMask(masters, XI_GestureSwipeEnd);
        XISetMask(masters, XI_DeviceChanged);
        XISetMask(devices, XI_HierarchyChanged);
    }
    /* Wildcard selectors are distinct from the explicit master selectors used
     * for raw-button observation. Never replace those masks. */
    trap_errors(dpy);
    XIUngrabSwipeGestureBegin(dpy, XIAllMasterDevices, root, 1, &mods);
    status = XISelectEvents(dpy, root, masks, 2);
    /* Root passive grab: apps that select swipes on their own window
     * (Qt6, e.g. Telegram) would otherwise take them before root. */
    if (SWIPE_ENABLED && status == Success) {
        XIEventMask grab_mask = {XIAllMasterDevices, sizeof masters, masters};
        mods.status = XIGrabSuccess;
        grab = XIGrabSwipeGestureBegin(dpy, XIAllMasterDevices, root, XIGrabModeAsync,
                                       XIGrabModeAsync, False, &grab_mask, 1, &mods);
        if (grab == 0 && mods.status != XIGrabSuccess) grab = 1;
    }
    clean = untrap_errors(dpy);
    active = SWIPE_ENABLED && status == Success && clean;
    if (SWIPE_DEBUG) fprintf(stderr, "daniwm: swipe configure XI=%d.%d supported=%d enabled=%d active=%d status=%d grab=%d trap-clean=%d\n", xi_major, xi_minor, supported, SWIPE_ENABLED, active, status, grab, clean);
}

void gestures_init(int major, int minor)
{
    xi_major = major; xi_minor = minor;
    supported = major > 2 || (major == 2 && minor >= 4);
    gestures_configure();
}

void gestures_event(XGenericEventCookie *cookie)
{
    XIGestureSwipeEvent *ev;
    SwipeEvent input;
    Swipe ended;
    SwipeDirection direction;
    int target, current, open;
    const char *action = "unchanged";
    if (!cookie || !cookie->data) return;
    if (cookie->evtype == XI_HierarchyChanged || cookie->evtype == XI_DeviceChanged) {
        if (SWIPE_DEBUG) {
            if (cookie->evtype == XI_DeviceChanged) {
                XIDeviceChangedEvent *changed = cookie->data;
                fprintf(stderr, "daniwm: swipe device-change device=%d source=%d reason=%d\n", changed->deviceid, changed->sourceid, changed->reason);
            } else {
                XIHierarchyEvent *changed = cookie->data;
                fprintf(stderr, "daniwm: swipe hierarchy-change flags=%d count=%d\n", changed->flags, changed->num_info);
            }
        }
        gestures_reset(cookie->evtype == XI_DeviceChanged ? "device-change" : "hierarchy-change");
        return;
    }
    if (!active || !SWIPE_ENABLED || drag.win != None) {
        gestures_reset(drag.win != None ? "drag" : "inactive");
        return;
    }
    if (cookie->evtype != XI_GestureSwipeBegin &&
        cookie->evtype != XI_GestureSwipeUpdate &&
        cookie->evtype != XI_GestureSwipeEnd) return;
    ev = cookie->data;
    input = (SwipeEvent){
        .phase = cookie->evtype == XI_GestureSwipeBegin ? SWIPE_BEGIN :
                 cookie->evtype == XI_GestureSwipeUpdate ? SWIPE_UPDATE : SWIPE_END,
        .device = ev->deviceid, .source = ev->sourceid, .fingers = ev->detail,
        .dx = ev->delta_x, .dy = ev->delta_y,
        .cancelled = !!(ev->flags & XIGestureSwipeEventCancelled)
    };
    if (SWIPE_DEBUG && input.phase == SWIPE_BEGIN && !swipe.active)
        fprintf(stderr, "daniwm: swipe BEGIN device=%d source=%d fingers=%d\n", input.device, input.source, input.fingers);
    direction = swipe_feed_report(&swipe, &input, SWIPE_THRESHOLD, &ended);
    open = overview_is_open();
    current = open ? overview_selection() : curws;
    target = swipe_target(current, NWS, direction, SWIPE_WRAP, SWIPE_REVERSE);
    if (direction == SWIPE_UP) {
        overview_show(0);
        action = "overview-show";
    } else if (direction == SWIPE_DOWN) {
        overview_hide(0);
        action = "overview-hide";
    } else if (target >= 0 && direction != SWIPE_NONE) {
        if (open) {
            overview_select(target);
            action = "overview-select";
        } else if (target != curws) {
            last_evtime = ev->time;
            view(target);
            action = "changed";
        } else {
            action = "clamped";
        }
    }
    if (SWIPE_DEBUG && ended.active)
        fprintf(stderr, "daniwm: swipe END device=%d source=%d dx=%g dy=%g vertical_travel=%g threshold=%g reason=%s direction=%d current=%d target=%d %s\n",
                ended.device, ended.source, ended.total_dx, ended.total_dy,
                ended.total_vertical, ended.threshold, ended.reason, direction,
                current, target, action);
}
#else
void gestures_init(int major, int minor) { (void)major; (void)minor; gestures_configure(); }
void gestures_reset(const char *reason) { (void)reason; }
void gestures_configure(void) {
    if (SWIPE_DEBUG) fprintf(stderr, "daniwm: swipe configure compiled=0 supported=0 enabled=%d active=0 status=not-attempted trap=not-attempted\n", SWIPE_ENABLED);
}
void gestures_event(XGenericEventCookie *cookie) { (void)cookie; }
#endif
