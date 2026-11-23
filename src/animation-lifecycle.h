/* Hide reasons: root ClientMessage/32 sent just before the WM unmaps a client.
 * Words: version, supporting WM, client, sequence, reason. */
#ifndef DANIWM_ANIMATION_LIFECYCLE_H
#define DANIWM_ANIMATION_LIFECYCLE_H
#include <stdint.h>
#define ANIMATION_REASON_ATOM "_DANIWM_ANIMATION_HIDE_V1"
#define ANIMATION_CLOSE_ATOM "_DANIWM_ANIMATION_CLOSE_V1"
#define ANIMATION_CLOSE_DEFAULT 100
#define ANIMATION_REASON_VERSION 1
#define ANIMATION_HIDE_WORKSPACE 1
#define ANIMATION_HIDE_MONOCLE 2
#define ANIMATION_HIDE_SCRATCH 3
#define ANIMATION_HIDE_RELOAD 4
static inline int animation_reason_valid(const unsigned long *v, unsigned long owner,
                                         uint32_t previous) {
    return v && owner && v[0] == ANIMATION_REASON_VERSION && v[1] == owner &&
        v[2] && v[2] <= UINT32_MAX && v[3] && v[3] <= UINT32_MAX &&
        (uint32_t)(v[3] - previous) != 0 &&
        (!previous || (uint32_t)(v[3] - previous) <= INT32_MAX) &&
        v[4] >= ANIMATION_HIDE_WORKSPACE && v[4] <= ANIMATION_HIDE_RELOAD;
}
static inline int animation_close_valid(const unsigned long *v, unsigned long n,
                                        unsigned long extra, unsigned long owner) {
    return v && owner && n == 3 && !extra && v[0] == 1 && v[1] == owner && v[2] <= 1000;
}
#endif
