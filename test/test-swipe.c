/* Offline: cc -std=c99 -Wall -Wextra -Werror -pedantic -Isrc
 * src/swipe.c test/test-swipe.c -lm -o /tmp/test-swipe && /tmp/test-swipe */
#include "swipe.h"

#include <assert.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>

static Swipe s;
static SwipeEvent event;

static SwipeDirection
send(SwipePhase phase, double dx, double dy)
{
    event.phase = phase;
    event.dx = dx;
    event.dy = dy;
    return swipe_feed(&s, &event, 100);
}

static void
begin(double threshold)
{
    swipe_reset(&s);
    event = (SwipeEvent){ .phase = SWIPE_BEGIN, .device = 7, .source = 12,
                          .fingers = 3 };
    assert(swipe_feed(&s, &event, threshold) == SWIPE_NONE);
}

static void
basic(void)
{
    begin(100);
    assert(send(SWIPE_UPDATE, 60, 0) == SWIPE_NONE);
    assert(send(SWIPE_END, 40, 0) == SWIPE_RIGHT);
    assert(!s.active);
    assert(send(SWIPE_END, 100, 0) == SWIPE_NONE);
    begin(100);
    assert(send(SWIPE_END, -100, 0) == SWIPE_LEFT);
    begin(100);
    assert(send(SWIPE_END, 99.99, 0) == SWIPE_NONE);
    begin(100);
    assert(send(SWIPE_END, 100, 50) == SWIPE_RIGHT);
    begin(100);
    assert(send(SWIPE_END, 100, -50.01) == SWIPE_NONE);
    begin(100);
    assert(send(SWIPE_END, 0, 1000) == SWIPE_DOWN);
    begin(100);
    assert(send(SWIPE_UPDATE, 1000, 0) == SWIPE_NONE);
    assert(send(SWIPE_END, 0, 0) == SWIPE_RIGHT);
    /* Begin deltas do not contribute; threshold is captured, not updated. */
    swipe_reset(&s);
    event = (SwipeEvent){SWIPE_BEGIN, 7, 12, 3, 1000, 0, 0};
    assert(swipe_feed(&s, &event, 150) == SWIPE_NONE);
    assert(send(SWIPE_END, 100, 0) == SWIPE_NONE);
}

static void
identity_and_lifecycle(void)
{
    begin(100);
    event.device++;
    assert(send(SWIPE_END, 500, 0) == SWIPE_NONE);
    assert(s.active);
    event.device--;
    event.source++;
    assert(send(SWIPE_UPDATE, NAN, NAN) == SWIPE_NONE);
    assert(send(SWIPE_END, 500, 0) == SWIPE_NONE);
    event.source--;
    assert(send(SWIPE_END, 100, 0) == SWIPE_RIGHT);
    for (int fingers = 0; fingers <= 5; ++fingers) {
        if (fingers == 3)
            continue;
        begin(100);
        event.fingers = fingers;
        assert(send(SWIPE_BEGIN, 0, 0) == SWIPE_NONE);
        event.fingers = 3;
        assert(send(SWIPE_END, 100, 0) == SWIPE_NONE);
        /* Invalid initial finger count remains rejected. */
        swipe_reset(&s);
        event.fingers = fingers;
        assert(send(SWIPE_BEGIN, 0, 0) == SWIPE_NONE);
        event.fingers = 3;
        assert(send(SWIPE_END, 100, 0) == SWIPE_NONE);
        begin(100);
        event.fingers = fingers;
        assert(send(SWIPE_UPDATE, 100, 0) == SWIPE_NONE);
        event.fingers = 3;
        assert(send(SWIPE_END, 100, 0) == SWIPE_NONE);
        begin(100);
        event.fingers = fingers;
        assert(send(SWIPE_END, 100, 0) == SWIPE_NONE);
    }
    for (int foreign = 0; foreign <= 1; ++foreign) {
        begin(100);
        event.device += foreign;
        assert(send(SWIPE_BEGIN, 0, 0) == SWIPE_NONE);
        event.device -= foreign;
        assert(send(SWIPE_END, 100, 0) == SWIPE_NONE);
        assert(!s.active);
    }
    for (int phase = SWIPE_BEGIN; phase <= SWIPE_END; ++phase) {
        begin(100);
        if (phase == SWIPE_BEGIN)
            swipe_reset(&s);
        event.cancelled = 1;
        assert(send((SwipePhase)phase, 100, 0) == SWIPE_NONE);
        event.cancelled = 0;
        assert(send(SWIPE_END, 100, 0) == SWIPE_NONE);
    }
    begin(100);
    assert(send(SWIPE_UPDATE, 100, 0) == SWIPE_NONE);
    swipe_reset(&s);
    assert(send(SWIPE_END, 100, 0) == SWIPE_NONE);
    assert(send(SWIPE_UPDATE, 100, 0) == SWIPE_NONE);
    assert(send(SWIPE_BEGIN, 0, 0) == SWIPE_NONE);
    assert(send(SWIPE_END, 100, 0) == SWIPE_RIGHT);
}

static void
reversals(void)
{
    for (int sign = -1; sign <= 1; sign += 2) {
        begin(100);
        assert(send(SWIPE_UPDATE, sign * 120, 0) == SWIPE_NONE);
        assert(send(SWIPE_UPDATE, -sign * 10, 0) == SWIPE_NONE);
        assert(send(SWIPE_END, 0, 0) != SWIPE_NONE);
        begin(100);
        assert(send(SWIPE_UPDATE, sign * 120, 0) == SWIPE_NONE);
        assert(send(SWIPE_UPDATE, -sign * 6, 0) == SWIPE_NONE);
        assert(send(SWIPE_UPDATE, -sign * 5, 0) == SWIPE_NONE);
        assert(send(SWIPE_END, sign * 200, 0) == SWIPE_NONE);
        /* Minor-axis drift must not veto an otherwise horizontal stroke.
         * Live logs showed large horizontal swipes rejected as axis-reversal
         * despite vertical travel well below half the horizontal distance. */
        begin(80);
        assert(send(SWIPE_UPDATE, sign * 50, 10) == SWIPE_NONE);
        assert(send(SWIPE_UPDATE, sign * 50, -10) == SWIPE_NONE);
        assert(send(SWIPE_END, 0, 0) ==
               (sign < 0 ? SWIPE_LEFT : SWIPE_RIGHT));
        begin(100);
        assert(send(SWIPE_UPDATE, sign * 100, 20) == SWIPE_NONE);
        assert(send(SWIPE_UPDATE, sign * 100, -11) == SWIPE_NONE);
        assert(send(SWIPE_END, sign * 100, 20) ==
               (sign < 0 ? SWIPE_LEFT : SWIPE_RIGHT));
        /* Early opposite jitter below allowance is harmless. */
        begin(100);
        assert(send(SWIPE_UPDATE, -sign * 5, 1) == SWIPE_NONE);
        assert(send(SWIPE_UPDATE, sign * 110, -1) == SWIPE_NONE);
        assert(send(SWIPE_END, 0, 0) != SWIPE_NONE);
    }
    /* Vertical travel, not vertical net, controls the 2:1 test. */
    begin(100);
    for (int i = 0; i < 6; ++i)
        assert(send(SWIPE_UPDATE, 0, i % 2 ? -10 : 10) == SWIPE_NONE);
    assert(send(SWIPE_END, 100, 0) == SWIPE_NONE);
    /* Minimum allowance is 2 even for a low threshold. */
    begin(10);
    assert(send(SWIPE_UPDATE, 14, 0) == SWIPE_NONE);
    assert(send(SWIPE_END, -2, 0) == SWIPE_RIGHT);
    begin(10);
    assert(send(SWIPE_UPDATE, 14, 0) == SWIPE_NONE);
    assert(send(SWIPE_END, -2.01, 0) == SWIPE_NONE);
}

static void
vertical(void)
{
    assert(SWIPE_LEFT == 1 && SWIPE_RIGHT == 2);
    for (int sign = -1; sign <= 1; sign += 2) {
        SwipeDirection expected = sign < 0 ? SWIPE_UP : SWIPE_DOWN;
        begin(100);
        assert(send(SWIPE_UPDATE, 0, sign * 60) == SWIPE_NONE);
        assert(send(SWIPE_END, 0, sign * 40) == expected);
        assert(send(SWIPE_END, 0, sign * 100) == SWIPE_NONE);
        begin(100);
        assert(send(SWIPE_END, 0, sign * 99.99) == SWIPE_NONE);
        begin(100);
        assert(send(SWIPE_END, 50, sign * 100) == expected);
        begin(100);
        assert(send(SWIPE_END, 50.01, sign * 100) == SWIPE_NONE);
        begin(100);
        assert(send(SWIPE_END, sign * 100, sign * 100) == SWIPE_NONE);
        /* Minor-axis reversals are harmless within total travel budget. */
        begin(100);
        assert(send(SWIPE_UPDATE, 20, sign * 100) == SWIPE_NONE);
        assert(send(SWIPE_END, -20, sign * 100) == expected);
        begin(100);
        assert(send(SWIPE_UPDATE, 30, sign * 50) == SWIPE_NONE);
        assert(send(SWIPE_END, -30, sign * 50) == SWIPE_NONE);
        begin(100);
        assert(send(SWIPE_UPDATE, 0, sign * 120) == SWIPE_NONE);
        assert(send(SWIPE_END, 0, -sign * 10) == expected);
        begin(100);
        assert(send(SWIPE_UPDATE, 0, sign * 120) == SWIPE_NONE);
        assert(send(SWIPE_UPDATE, 0, -sign * 11) == SWIPE_NONE);
        assert(send(SWIPE_END, 0, sign * 200) == SWIPE_NONE);
        begin(100);
        event.cancelled = 1;
        assert(send(SWIPE_END, 0, sign * 100) == SWIPE_NONE);
        begin(100);
        event.fingers = 2;
        assert(send(SWIPE_END, 0, sign * 100) == SWIPE_NONE);
    }
    assert(swipe_target(0, 3, SWIPE_UP, 1, 1) == -1);
    assert(swipe_target(0, 3, SWIPE_DOWN, 1, 1) == -1);
}

static void
nonfinite(void)
{
    const double invalid[] = {NAN, INFINITY, -INFINITY};
    const double bad_threshold[] = {0, -1, NAN, INFINITY, -INFINITY};
    for (unsigned i = 0; i < sizeof(bad_threshold) / sizeof(*bad_threshold); ++i) {
        begin(bad_threshold[i]);
        assert(send(SWIPE_END, 1000, 0) == SWIPE_NONE);
    }
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
        for (int axis = 0; axis < 2; ++axis) {
            begin(100);
            assert(send(SWIPE_UPDATE, axis ? 0 : invalid[i], axis ? invalid[i] : 0) == SWIPE_NONE);
            assert(send(SWIPE_END, 100, 0) == SWIPE_NONE);
            begin(100);
            assert(send(SWIPE_END, axis ? 100 : invalid[i], axis ? invalid[i] : 0) == SWIPE_NONE);
        }
    }
    begin(100);
    assert(send(SWIPE_UPDATE, DBL_MAX, 0) == SWIPE_NONE);
    assert(send(SWIPE_END, DBL_MAX, 0) == SWIPE_NONE);
    begin(100);
    assert(send(SWIPE_END, DBL_MAX, DBL_MAX) == SWIPE_NONE);
}

static void
targets(void)
{
    for (int count = 1; count <= 6; ++count)
        for (int current = 0; current < count; ++current)
            for (int wrap = 0; wrap < 2; ++wrap)
                for (int reverse = 0; reverse < 2; ++reverse)
                    for (int d = SWIPE_NONE; d <= SWIPE_RIGHT; ++d) {
                        int step = d == SWIPE_NONE ? 0 : d == SWIPE_LEFT ? 1 : -1;
                        int expected;
                        if (reverse)
                            step = -step;
                        expected = current + step;
                        if (wrap)
                            expected = (expected + count) % count;
                        else if (expected < 0)
                            expected = 0;
                        else if (expected >= count)
                            expected = count - 1;
                        assert(swipe_target(current, count, (SwipeDirection)d, wrap, reverse) == expected);
                    }
    assert(swipe_target(0, 0, SWIPE_LEFT, 0, 0) == -1);
    assert(swipe_target(0, -1, SWIPE_LEFT, 0, 0) == -1);
    assert(swipe_target(-1, 3, SWIPE_LEFT, 0, 0) == -1);
    assert(swipe_target(3, 3, SWIPE_LEFT, 0, 0) == -1);
    assert(swipe_target(0, 3, (SwipeDirection)99, 0, 0) == -1);
    assert(swipe_target(INT_MAX - 1, INT_MAX, SWIPE_LEFT, 1, 0) == 0);
}

int
main(void)
{
    basic();
    identity_and_lifecycle();
    reversals();
    vertical();
    nonfinite();
    targets();
    puts("swipe: all offline tests passed");
    return 0;
}
