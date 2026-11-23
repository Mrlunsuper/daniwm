/* Best-effort layout transaction: real geometry is final at once; the
 * compositor only moves the picture. Bad transactions fall back to instant. */
#ifndef DANIWM_LAYOUT_ANIMATION_H
#define DANIWM_LAYOUT_ANIMATION_H

#define LAYOUT_ANIMATION_ATOM "_DANIWM_LAYOUT_ANIMATION_V1"
#define LAYOUT_ANIMATION_SETTINGS_ATOM "_DANIWM_ANIMATION_LAYOUT_SETTINGS_V1"
#define LAYOUT_ANIMATION_VERSION 1
#define LAYOUT_ANIMATION_SETTINGS_VERSION 1
#define LAYOUT_ANIMATION_HEADER_WORDS 5
#define LAYOUT_ANIMATION_DEFAULT 160
#define LAYOUT_ANIMATION_MAX_MS 1000
#define LAYOUT_ANIMATION_MAX_CLIENTS 64
#define LAYOUT_ANIMATION_ENTRY_WORDS 5
#define LAYOUT_ANIMATION_MAX_WORDS (LAYOUT_ANIMATION_HEADER_WORDS + \
    LAYOUT_ANIMATION_MAX_CLIENTS * LAYOUT_ANIMATION_ENTRY_WORDS)

static inline int layout_animation_settings_valid(const unsigned long *p,
                                                   unsigned long n,
                                                   unsigned long extra,
                                                   unsigned long owner) {
    return p && n == 3 && !extra && owner &&
        p[0] == LAYOUT_ANIMATION_SETTINGS_VERSION && p[1] == owner &&
        p[2] <= LAYOUT_ANIMATION_MAX_MS;
}

#endif
