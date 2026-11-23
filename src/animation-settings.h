/* Animation settings: root CARDINAL/32, five words: version, supporting WM,
 * enabled, reduced motion, open duration. */
#ifndef DANIWM_ANIMATION_SETTINGS_H
#define DANIWM_ANIMATION_SETTINGS_H
#define ANIMATION_SETTINGS_ATOM "_DANIWM_ANIMATION_SETTINGS_V1"
#define ANIMATION_CONTROL_ATOM "_DANIWM_ANIMATION_CONTROL_V1"
#define ANIMATION_SETTINGS_WORDS 5
#define ANIMATION_SETTINGS_VERSION 1
#define ANIMATION_OPEN_DEFAULT 140

static inline int animation_settings_valid(const unsigned long *p, unsigned long n,
                                           unsigned long extra, unsigned long owner) {
    return p && n == ANIMATION_SETTINGS_WORDS && !extra && owner &&
        p[0] == ANIMATION_SETTINGS_VERSION && p[1] == owner &&
        p[2] <= 1 && p[3] <= 1 && p[4] <= 1000;
}
#endif
