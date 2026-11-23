/* Phase 3-P1 experiment only: workspace capture transaction and scene state.
 * Test atoms, never linked into daniwm or dani-comp. */
#ifndef TEST_WS_CAPTURE_PROTOTYPE_H
#define TEST_WS_CAPTURE_PROTOTYPE_H
#include <stddef.h>
#include <stdint.h>

#define WSP_VERSION 1
#define WSP_MEMBERS 16
#define WSP_HEADER 10
#define WSP_WORDS (WSP_HEADER + WSP_MEMBERS)
#define WSP_TTL_MS 1000
#define WSP_MAX_DEADLINE_MS 1000
#define WSP_BUDGET ((uint64_t)32 * 1024 * 1024)
#define WSP_PREPARE 1
#define WSP_CANCEL 2
#define WSP_CAP_ATOM "_DANIWM_TEST_WS_CAP_V1"     /* model owner window: version */
#define WSP_COMP_ATOM "_DANIWM_TEST_WS_COMP_V1"   /* root: model owner window */
#define WSP_OWNER_ATOM "_DANIWM_TEST_WS_OWNER_V1" /* root: WM owner window */
#define WSP_EPOCH_ATOM "_DANIWM_TEST_WS_EPOCH_V1" /* root: monitor epoch */
#define WSP_TXN_ATOM "_DANIWM_TEST_WS_TXN_V1"     /* root: transaction words */
#define WSP_EVENT_ATOM "_DANIWM_TEST_WS_EVENT_V1" /* root ClientMessage */

enum {
    WSP_OK, WSP_INVALID, WSP_STALE, WSP_OVERWRITTEN, WSP_LATE, WSP_UNNAMED,
    WSP_BUDGET_EXCEEDED, WSP_OWNER, WSP_EPOCH, WSP_EXPIRED, WSP_SUPERSEDED,
    WSP_CANCELLED
};
static const char *const wsp_reason_names[] = {
    "ok", "invalid", "stale", "overwritten", "late", "unnamed", "budget",
    "owner", "epoch", "expired", "superseded", "cancelled"
};

/* Words: version, wm, compositor, epoch, sequence, from, to, sent_ms,
 * deadline_ms, count, then count member XIDs. */
typedef struct {
    uint32_t wm, compositor, epoch, sequence, from, to, sent_ms, deadline_ms, count;
    uint32_t members[WSP_MEMBERS];
} WSTxn;

static inline size_t wsp_encode(unsigned long *v, const WSTxn *t) {
    v[0] = WSP_VERSION; v[1] = t->wm; v[2] = t->compositor; v[3] = t->epoch;
    v[4] = t->sequence; v[5] = t->from; v[6] = t->to; v[7] = t->sent_ms;
    v[8] = t->deadline_ms; v[9] = t->count;
    for (uint32_t i = 0; i < t->count && i < WSP_MEMBERS; i++) v[WSP_HEADER + i] = t->members[i];
    return WSP_HEADER + (t->count < WSP_MEMBERS ? t->count : WSP_MEMBERS);
}
static inline int wsp_decode(WSTxn *t, const unsigned long *v, size_t n, unsigned long extra,
                             uint32_t wm, uint32_t compositor, uint32_t epoch) {
    if (!t || !v || extra || n < WSP_HEADER || n > WSP_WORDS || !wm || !compositor) return 0;
    for (size_t i = 0; i < n; i++) if (v[i] > UINT32_MAX) return 0;
    if (v[0] != WSP_VERSION || v[1] != wm || v[2] != compositor || v[3] != epoch ||
        !v[4] || v[5] == v[6] || !v[8] || v[8] > WSP_MAX_DEADLINE_MS ||
        v[9] > WSP_MEMBERS || n != WSP_HEADER + v[9]) return 0;
    WSTxn r = {0};
    r.wm = wm; r.compositor = compositor; r.epoch = epoch;
    r.sequence = (uint32_t)v[4]; r.from = (uint32_t)v[5]; r.to = (uint32_t)v[6];
    r.sent_ms = (uint32_t)v[7]; r.deadline_ms = (uint32_t)v[8]; r.count = (uint32_t)v[9];
    for (uint32_t i = 0; i < r.count; i++) {
        r.members[i] = (uint32_t)v[WSP_HEADER + i];
        if (!r.members[i]) return 0;
        for (uint32_t j = 0; j < i; j++) if (r.members[j] == r.members[i]) return 0;
    }
    *t = r;
    return 1;
}
/* Serial order with wrap: nonzero distance of at most INT32_MAX. */
static inline int wsp_newer(uint32_t a, uint32_t b) {
    uint32_t d = a - b;
    return a && d && d <= INT32_MAX;
}
/* Both processes run on one host and use CLOCK_MONOTONIC (low 32 bits). */
static inline int wsp_age_ok(uint32_t sent_ms, uint32_t now_ms, uint32_t deadline_ms) {
    return (uint32_t)(now_ms - sent_ms) <= deadline_ms;
}
/* Server image bytes from the pixmap format (bits per pixel, scanline pad). */
static inline int wsp_image_bytes(uint64_t *out, unsigned w, unsigned h, unsigned bpp, unsigned pad) {
    if (!out || !w || !h || w > 32767 || h > 32767 || !bpp || bpp > 32 || !pad || pad % 8 ||
        pad > 128) return 0;
    uint64_t bits = (uint64_t)w * bpp;
    uint64_t stride = (bits + pad - 1) / pad * (pad / 8);
    *out = stride * h;
    return 1;
}

enum { WSP_IDLE, WSP_COLLECTING, WSP_READY };
typedef struct {
    uint32_t id;
    uint64_t generation;
    unsigned long pixmap;
    uint64_t bytes;
} WSMember;
typedef struct {
    int phase;
    WSTxn txn;
    WSMember m[WSP_MEMBERS];
    unsigned retained;
    uint64_t bytes;
    int64_t prepared_ms;
    int committed; /* the target desktop is already published */
} WSScene;
typedef void (*WSRelease)(unsigned long pixmap);

static inline void wsp_clear(WSScene *s, WSRelease release) {
    for (unsigned i = 0; i < WSP_MEMBERS; i++)
        if (s->m[i].pixmap && release) release(s->m[i].pixmap);
    *s = (WSScene){0};
}
/* Admission before any retention: the sum of member images fits the budget. */
static inline int wsp_begin(WSScene *s, const WSTxn *t, const uint64_t *bytes,
                            const uint64_t *generations, int64_t now_ms) {
    uint64_t total = 0;
    for (uint32_t i = 0; i < t->count; i++) {
        if (bytes[i] > WSP_BUDGET || total > WSP_BUDGET - bytes[i]) return 0;
        total += bytes[i];
    }
    *s = (WSScene){0};
    s->phase = t->count ? WSP_COLLECTING : WSP_READY;
    s->txn = *t;
    s->bytes = total;
    s->prepared_ms = now_ms;
    for (uint32_t i = 0; i < t->count; i++) {
        s->m[i].id = t->members[i];
        s->m[i].generation = generations[i];
        s->m[i].bytes = bytes[i];
    }
    return 1;
}
static inline int wsp_member(const WSScene *s, uint32_t id) {
    if (s->phase == WSP_IDLE) return -1;
    for (uint32_t i = 0; i < s->txn.count; i++) if (s->m[i].id == id) return (int)i;
    return -1;
}
/* Keep one member image. The generation must match the one at PREPARE. */
static inline int wsp_retain(WSScene *s, int index, unsigned long pixmap, uint64_t generation) {
    if (s->phase != WSP_COLLECTING || index < 0 || (uint32_t)index >= s->txn.count ||
        !pixmap || s->m[index].pixmap || s->m[index].generation != generation) return 0;
    s->m[index].pixmap = pixmap;
    if (++s->retained == s->txn.count) s->phase = WSP_READY;
    return 1;
}
static inline int wsp_expired(const WSScene *s, int64_t now_ms) {
    return s->phase != WSP_IDLE && now_ms - s->prepared_ms > WSP_TTL_MS;
}
#endif
