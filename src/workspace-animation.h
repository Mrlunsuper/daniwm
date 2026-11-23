/* Workspace crossfade: settings and the PREPARE transaction (daniwm -> dani-comp).
 * Transaction words: version, WM owner, monitor epoch, sequence, from, to,
 * sent time (CLOCK_MONOTONIC ms, low 32 bits), count, then member XIDs. */
#ifndef DANIWM_WORKSPACE_ANIMATION_H
#define DANIWM_WORKSPACE_ANIMATION_H
#include <stdint.h>
#include "exit-layer.h"

#define WORKSPACE_ANIMATION_ATOM "_DANIWM_WORKSPACE_V1"
#define WORKSPACE_SETTINGS_ATOM "_DANIWM_ANIMATION_WORKSPACE_SETTINGS_V1"
#define MONITOR_EPOCH_ATOM "_DANIWM_MONITOR_EPOCH_V1"
#define WORKSPACE_ANIMATION_VERSION 1
#define WORKSPACE_ANIMATION_MAX_MS 250
#define WORKSPACE_ANIMATION_DEADLINE_MS 100
#define WORKSPACE_ANIMATION_HEADER 8
#define WORKSPACE_ANIMATION_MEMBERS EXIT_COUNT /* the shared image pool */
#define WORKSPACE_ANIMATION_WORDS (WORKSPACE_ANIMATION_HEADER + WORKSPACE_ANIMATION_MEMBERS)

typedef struct {
    uint32_t sequence, from, to, sent_ms, count;
    uint32_t members[WORKSPACE_ANIMATION_MEMBERS];
} WorkspaceTxn;

static inline int workspace_settings_valid(const unsigned long *p, unsigned long n,
                                           unsigned long extra, unsigned long owner) {
    return p && n == 3 && !extra && owner && p[0] == WORKSPACE_ANIMATION_VERSION &&
        p[1] == owner && p[2] <= WORKSPACE_ANIMATION_MAX_MS;
}
static inline int workspace_txn_decode(WorkspaceTxn *t, const unsigned long *v, unsigned long n,
                                       unsigned long extra, unsigned long owner,
                                       unsigned long epoch) {
    if (!t || !v || extra || !owner || n < WORKSPACE_ANIMATION_HEADER ||
        n > WORKSPACE_ANIMATION_WORDS) return 0;
    for (unsigned long i = 0; i < n; i++) if (v[i] > UINT32_MAX) return 0;
    if (v[0] != WORKSPACE_ANIMATION_VERSION || v[1] != owner || v[2] != epoch || !v[3] ||
        v[4] == v[5] || v[7] > WORKSPACE_ANIMATION_MEMBERS ||
        n != WORKSPACE_ANIMATION_HEADER + v[7]) return 0;
    WorkspaceTxn r = {0};
    r.sequence = (uint32_t)v[3]; r.from = (uint32_t)v[4]; r.to = (uint32_t)v[5];
    r.sent_ms = (uint32_t)v[6]; r.count = (uint32_t)v[7];
    for (uint32_t i = 0; i < r.count; i++) {
        r.members[i] = (uint32_t)v[WORKSPACE_ANIMATION_HEADER + i];
        if (!r.members[i]) return 0;
        for (uint32_t j = 0; j < i; j++) if (r.members[j] == r.members[i]) return 0;
    }
    *t = r;
    return 1;
}
/* Serial order with wrap; zero is never a valid sequence. */
static inline int workspace_sequence_newer(uint32_t seq, uint32_t last) {
    uint32_t d = seq - last;
    return seq && d && d <= INT32_MAX;
}
#endif
