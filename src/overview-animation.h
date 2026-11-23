/* Overview fade: settings and the overview window role (daniwm -> dani-comp).
 * Settings words: version, WM owner, duration. Role words: version, WM owner,
 * overview window XID. */
#ifndef DANIWM_OVERVIEW_ANIMATION_H
#define DANIWM_OVERVIEW_ANIMATION_H
#include <stdint.h>

#define OVERVIEW_SETTINGS_ATOM "_DANIWM_ANIMATION_OVERVIEW_SETTINGS_V1"
#define OVERVIEW_ROLE_ATOM "_DANIWM_OVERVIEW_V1"
#define OVERVIEW_ANIMATION_VERSION 1
#define OVERVIEW_ANIMATION_MAX_MS 250

static inline int overview_settings_valid(const unsigned long *p, unsigned long n,
                                          unsigned long extra, unsigned long owner) {
    return p && n == 3 && !extra && owner && p[0] == OVERVIEW_ANIMATION_VERSION &&
        p[1] == owner && p[2] <= OVERVIEW_ANIMATION_MAX_MS;
}
/* Returns the overview window, or 0 when the role is invalid. */
static inline unsigned long overview_role_window(const unsigned long *p, unsigned long n,
                                                 unsigned long extra, unsigned long owner) {
    if (!p || n != 3 || extra || !owner || p[0] != OVERVIEW_ANIMATION_VERSION ||
        p[1] != owner || !p[2] || p[2] > UINT32_MAX) return 0;
    return p[2];
}
#endif
