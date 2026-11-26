/* Rounded window corners: settings (daniwm -> dani-comp).
 * Words: version, WM owner, radius in px (0 = off). */
#ifndef DANIWM_CORNER_SETTINGS_H
#define DANIWM_CORNER_SETTINGS_H

#define CORNER_SETTINGS_ATOM "_DANIWM_CORNER_SETTINGS_V1"
#define CORNER_SETTINGS_VERSION 1
#define CORNER_RADIUS_MAX 24

static inline int corner_settings_valid(const unsigned long *p, unsigned long n,
                                        unsigned long extra, unsigned long owner) {
    return p && n == 3 && !extra && owner && p[0] == CORNER_SETTINGS_VERSION &&
        p[1] == owner && p[2] <= CORNER_RADIUS_MAX;
}
#endif
