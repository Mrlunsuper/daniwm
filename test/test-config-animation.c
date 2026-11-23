/* Offline Phase 1A scalar checks, using the real parser. */
#include "../src/config.c"
#include <assert.h>
static void scalar(const char *key, const char *value) {
    char k[64], v[128];
    snprintf(k, sizeof(k), "%s", key);
    snprintf(v, sizeof(v), "%s", value);
    parse_scalar(k, v);
}
int main(void) {
    assert(!COMPOSITOR && !ANIMATIONS && !REDUCE_MOTION);
    assert(ANIMATION_OPEN_MS == 140 && ANIMATION_CLOSE_MS == 100 && ANIMATION_LAYOUT_MS == 0);
    assert(ANIMATION_WORKSPACE_MS == 0);
    scalar("compositor", "yes"); assert(COMPOSITOR == 1);
    scalar("animations", "on"); assert(ANIMATIONS == 1);
    scalar("reduce_motion", "true"); assert(REDUCE_MOTION == 1);
    scalar("animations", "wrong"); assert(ANIMATIONS == 1);
    scalar("reduce_motion", "wrong"); assert(REDUCE_MOTION == 1);
    scalar("compositor", "wrong"); assert(COMPOSITOR == 1);
    const char *bad[] = {"", "nan", "inf", "-1", "1001", "1.5", "160ms", "1e2",
        "160 trailing", "9999999999999999999999999"};
    for (unsigned i = 0; i < sizeof(bad)/sizeof(bad[0]); i++) {
        scalar("animation_open_ms", bad[i]); assert(ANIMATION_OPEN_MS == 140);
        scalar("animation_close_ms", bad[i]); assert(ANIMATION_CLOSE_MS == 100);
        scalar("animation_layout_ms", bad[i]); assert(ANIMATION_LAYOUT_MS == 0);
        scalar("animation_workspace_ms", bad[i]); assert(ANIMATION_WORKSPACE_MS == 0);
    }
    scalar("animation_workspace_ms", "251"); assert(ANIMATION_WORKSPACE_MS == 0);
    scalar("animation_workspace_ms", "250"); assert(ANIMATION_WORKSPACE_MS == 250);
    scalar("animation_workspace_ms", "1000"); assert(ANIMATION_WORKSPACE_MS == 250);
    scalar("animation_workspace_ms", "0"); assert(ANIMATION_WORKSPACE_MS == 0);
    scalar("animation_close_ms", "0"); assert(ANIMATION_CLOSE_MS == 0);
    scalar("animation_close_ms", "1000"); assert(ANIMATION_CLOSE_MS == 1000);
    scalar("animation_close_ms", "100"); assert(ANIMATION_CLOSE_MS == 100);
    scalar("animation_open_ms", "0"); assert(ANIMATION_OPEN_MS == 0);
    scalar("animation_open_ms", "1000"); assert(ANIMATION_OPEN_MS == 1000);
    scalar("animation_open_ms", "140"); assert(ANIMATION_OPEN_MS == 140);
    scalar("animation_layout_ms", "160"); assert(ANIMATION_LAYOUT_MS == 160);
    scalar("animation_layout_ms", "0"); assert(ANIMATION_LAYOUT_MS == 0);
    scalar("animation_layout_ms", "1000"); assert(ANIMATION_LAYOUT_MS == 1000);
    scalar("animation_layout_ms", "0"); assert(ANIMATION_LAYOUT_MS == 0);
    scalar("compositor", "off"); assert(COMPOSITOR == 0);
    scalar("animations", "false"); assert(ANIMATIONS == 0);
    scalar("reduce_motion", "no"); assert(REDUCE_MOTION == 0);
    puts("PASS: Phase 1A config defaults, booleans, duration boundaries and rejection");
    return 0;
}
