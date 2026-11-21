/* Offline scalar parser regression: no X connection or gesture grabs. */
#include "../src/config.c"
#include <assert.h>

static void scalar(const char *key, const char *value) {
    char k[64], v[128];
    snprintf(k, sizeof(k), "%s", key);
    snprintf(v, sizeof(v), "%s", value);
    parse_scalar(k, v);
}

int main(void) {
    assert(SWIPE_ENABLED == 0 && SWIPE_THRESHOLD == 80.0);
    assert(SWIPE_WRAP == 0 && SWIPE_REVERSE == 0 && SWIPE_DEBUG == 0);
    scalar("swipe_debug", "yes"); assert(SWIPE_DEBUG == 1);
    scalar("swipe_debug", "invalid"); assert(SWIPE_DEBUG == 1);
    scalar("swipe_debug", "off"); assert(SWIPE_DEBUG == 0);
    scalar("swipe_debug", "1"); assert(SWIPE_DEBUG == 1);
    scalar("swipe_debug", "0"); assert(SWIPE_DEBUG == 0);
    const char *bad[] = { "", "nan", "NaN", "inf", "-inf", "0", "0.99",
        "10000.1", "1e999", "1e-999", "80px", "80 90", "80 " };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        scalar("swipe_threshold", bad[i]);
        assert(SWIPE_THRESHOLD == 80.0);
    }
    scalar("swipe_threshold", "1"); assert(SWIPE_THRESHOLD == 1.0);
    scalar("swipe_threshold", "10000"); assert(SWIPE_THRESHOLD == 10000.0);
    scalar("swipe_threshold", "123.5"); assert(SWIPE_THRESHOLD == 123.5);
    scalar("swipe_threshold", "invalid"); assert(SWIPE_THRESHOLD == 123.5);
    scalar("swipe_enabled", "yes"); assert(SWIPE_ENABLED == 1);
    scalar("swipe_wrap", "true"); assert(SWIPE_WRAP == 1);
    scalar("swipe_reverse", "on"); assert(SWIPE_REVERSE == 1);
    scalar("swipe_enabled", "invalid"); assert(SWIPE_ENABLED == 1);
    scalar("swipe_enabled", "0"); assert(SWIPE_ENABLED == 0);
    scalar("swipe_wrap", "off"); assert(SWIPE_WRAP == 0);
    scalar("swipe_reverse", "false"); assert(SWIPE_REVERSE == 0);
    puts("PASS: swipe config scalars");
    return 0;
}
