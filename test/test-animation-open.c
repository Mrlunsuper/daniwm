/* Phase 1A pure math/settings validation. Never opens a display. */
#include "../src/animation.h"
#include "../src/animation-settings.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

int main(void) {
    Animation a = {0};
    assert(animation_sample(&a, 42, 1, 0) == 1.0);
    animation_start(&a, 42, 1, 100, 160);
    assert(animation_value(&a, 42, 1, 100) == 0.0);
    assert(animation_sample(&a, 42, 1, 180) == 0.5 && a.active);
    assert(animation_sample(&a, 42, 1, 260) == 1.0 && !a.active);
    animation_start(&a, 42, 1, 100, 160);
    assert(animation_sample(&a, 42, 2, 110) == 1.0 && !a.active);
    animation_start(&a, 42, 2, 100, 160);
    assert(animation_sample(&a, 43, 2, 110) == 1.0 && !a.active);
    animation_start(&a, 42, 2, 100, 0);
    assert(!a.active && animation_sample(&a, 42, 2, 100) == 1.0);
    animation_start(&a, 42, 0, 100, 160); assert(!a.active);
    animation_start(&a, 0, 2, 100, 160); assert(!a.active);
    animation_start(&a, 42, 2, -1, 160); assert(!a.active);
    animation_start(&a, 42, 2, 100, 160);
    assert(animation_sample(&a, 42, 2, 99) == 1.0 && !a.active);
    animation_start(&a, 42, 2, 100, 160);
    assert(animation_sample(&a, 42, 2, INT64_MAX) == 1.0 && !a.active);
    animation_start(&a, 42, 2, 100, 160);
    animation_cancel(&a); assert(!a.active);
    animation_start(&a, 42, 3, 200, 100);
    assert(animation_sample(&a, 42, 3, 225) == 0.25);
    unsigned long values[] = {1, 42, 1, 0, 140};
    assert(animation_settings_valid(values, 5, 0, 42));
    assert(!animation_settings_valid(NULL, 5, 0, 42));
    assert(!animation_settings_valid(values, 4, 0, 42));
    assert(!animation_settings_valid(values, 6, 0, 42));
    assert(!animation_settings_valid(values, 5, 1, 42));
    assert(!animation_settings_valid(values, 5, 0, 0));
    assert(!animation_settings_valid(values, 5, 0, 43));
    for (unsigned i = 0; i < 5; i++) {
        unsigned long old = values[i];
        values[i] = ~0UL;
        assert(!animation_settings_valid(values, 5, 0, 42));
        values[i] = old;
    }
    values[4] = 0; assert(animation_settings_valid(values, 5, 0, 42));
    values[4] = 1000; assert(animation_settings_valid(values, 5, 0, 42));
    puts("PASS: Phase 1A linear fade, exact completion, cancellation, generations and bounded settings");
    return 0;
}
