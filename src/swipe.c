#include "swipe.h"

#include <math.h>

void
swipe_reset(Swipe *s)
{
    *s = (Swipe){0};
}

static int
axis_add(SwipeAxis *a, double delta, double allowance)
{
    a->position += delta;
    if (!isfinite(a->position))
        return 0;
    if (a->position < a->low)
        a->low = a->position;
    if (a->position > a->high)
        a->high = a->position;
    if (!a->direction) {
        if (a->position - a->low > allowance)
            a->direction = 1;
        else if (a->high - a->position > allowance)
            a->direction = -1;
    }
    if ((a->direction > 0 && a->high - a->position > allowance) ||
        (a->direction < 0 && a->position - a->low > allowance))
        a->reversed = 1;
    return 1;
}

static SwipeDirection
swipe_feed_impl(Swipe *s, const SwipeEvent *e, double threshold, Swipe *ended)
{
    SwipeDirection result = SWIPE_NONE;
    if (ended) *ended = (Swipe){0};
    if (e->phase == SWIPE_BEGIN) {
        if (s->active) {
            s->rejected = 1;
            if (!s->reason) s->reason = "overlapping-begin";
            return SWIPE_NONE;
        }
        swipe_reset(s);
        s->active = 1;
        s->device = e->device;
        s->source = e->source;
        s->threshold = threshold;
        s->rejected = e->fingers != 3 || e->cancelled ||
                      !isfinite(threshold) || threshold <= 0;
        if (s->rejected) s->reason = e->fingers != 3 ? "wrong-fingers" :
            e->cancelled ? "cancelled" : "invalid-begin";
        return SWIPE_NONE;
    }
    if (!s->active || e->device != s->device || e->source != s->source)
        return SWIPE_NONE;
    if (e->phase != SWIPE_UPDATE && e->phase != SWIPE_END) {
        s->rejected = 1;
        if (!s->reason) s->reason = "invalid-phase";
        return SWIPE_NONE;
    }
    s->total_dx += e->dx; s->total_dy += e->dy;
    s->total_vertical += fabs(e->dy);
    if (e->fingers != 3 || e->cancelled || !isfinite(e->dx) || !isfinite(e->dy)) {
        s->rejected = 1;
        if (!s->reason) s->reason = e->fingers != 3 ? "wrong-fingers" :
            e->cancelled ? "cancelled" : "invalid-delta";
    }
    if (!s->rejected) {
        double allowance = fmax(2.0, s->threshold * 0.1);
        int x_ok = axis_add(&s->x, e->dx, allowance);
        int y_ok = axis_add(&s->y, e->dy, allowance);
        s->vertical_travel += fabs(e->dy);
        s->horizontal_travel += fabs(e->dx);
        /* Reversals are provisional until End chooses the dominant axis. */
        if (!x_ok || !y_ok || !isfinite(s->vertical_travel) ||
            !isfinite(s->horizontal_travel)) {
            s->rejected = 1;
            s->reason = "numeric-overflow";
        }
    }
    if (e->phase == SWIPE_END) {
        int vertical = fabs(s->y.position) > fabs(s->x.position);
        SwipeAxis *dominant = vertical ? &s->y : &s->x;
        double distance = fabs(dominant->position);
        double perpendicular = vertical ? s->horizontal_travel : s->vertical_travel;
        if (!s->rejected) {
            if (dominant->reversed)
                s->reason = "axis-reversal";
            else if (distance < s->threshold)
                s->reason = "below-threshold";
            else if (perpendicular > distance * 0.5)
                s->reason = "too-vertical"; /* historical diagonal diagnostic */
            else
                result = vertical ? (dominant->position < 0 ? SWIPE_UP : SWIPE_DOWN) :
                                    (dominant->position < 0 ? SWIPE_LEFT : SWIPE_RIGHT);
        }
        if (result) s->reason = "accepted";
        if (ended) *ended = *s;
        swipe_reset(s);
    }
    return result;
}

SwipeDirection
swipe_feed(Swipe *s, const SwipeEvent *e, double threshold)
{
    return swipe_feed_impl(s, e, threshold, 0);
}

SwipeDirection
swipe_feed_report(Swipe *s, const SwipeEvent *e, double threshold, Swipe *ended)
{
    return swipe_feed_impl(s, e, threshold, ended);
}

int
swipe_target(int current, int count, SwipeDirection direction, int wrap, int reverse)
{
    int step;
    if (count <= 0 || current < 0 || current >= count ||
        (direction != SWIPE_NONE && direction != SWIPE_LEFT && direction != SWIPE_RIGHT))
        return -1;
    if (direction == SWIPE_NONE)
        return current;
    step = direction == SWIPE_LEFT ? 1 : -1;
    if (reverse)
        step = -step;
    if (step < 0)
        return current > 0 ? current - 1 : (wrap ? count - 1 : 0);
    return current < count - 1 ? current + 1 : (wrap ? 0 : count - 1);
}
