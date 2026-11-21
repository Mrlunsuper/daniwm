#pragma once

/* Pure classifier: no X types, allocation, clock, or global state. */
typedef enum { SWIPE_NONE, SWIPE_LEFT, SWIPE_RIGHT, SWIPE_UP, SWIPE_DOWN } SwipeDirection;
typedef enum { SWIPE_BEGIN, SWIPE_UPDATE, SWIPE_END } SwipePhase;

typedef struct {
    SwipePhase phase;
    int device, source, fingers;
    double dx, dy;
    int cancelled;
} SwipeEvent;

typedef struct {
    double position, low, high;
    int direction, reversed;
} SwipeAxis;

typedef struct {
    int active, rejected, device, source;
    double threshold, vertical_travel, horizontal_travel;
    double total_dx, total_dy, total_vertical;
    const char *reason; /* first rejection, or final decision; static strings */
    SwipeAxis x, y;
} Swipe;

/* Initialize/reset before use and on device removal, disable, or reconfigure. */
void swipe_reset(Swipe *swipe);
/* Threshold is captured at Begin; must be finite and positive. Begin has no
 * deltas. Valid Update and End deltas are consumed once; cancellation rejects.
 * Only a matching, uncancelled End can emit a direction. An overlapping Begin
 * poisons the current gesture until its matching End or an explicit reset.
 * Nonmatching Update/End events are ignored. Fingers must remain exactly 3.
 * Dominant net distance must meet threshold and be >= twice total perpendicular
 * travel. Dominant-axis reversal greater than max(2, threshold/10) rejects;
 * perpendicular direction changes alone do not reject a stroke.
 */
SwipeDirection swipe_feed(Swipe *swipe, const SwipeEvent *event, double threshold);
/* Optional pre-reset snapshot on matching End; otherwise active is zero. */
SwipeDirection swipe_feed_report(Swipe *swipe, const SwipeEvent *event,
                                 double threshold, Swipe *ended);
/* Returns a zero-based workspace index, or -1 for invalid current/count/direction.
 * UP/DOWN are invalid here (return -1). Left increments (next), right decrements (previous); reverse swaps this; wrap selects wrapping
 * rather than clamping. No action is represented by SWIPE_NONE (current).
 */
int swipe_target(int current, int count, SwipeDirection direction, int wrap, int reverse);
