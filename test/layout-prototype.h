/* Phase 2 experiment ONLY. No production atoms/API or runtime linkage.
 * One bounded transaction, no queue, no geometry delay while waiting READY.
 * Values are Xlib format-32 unsigned longs; reject non-wire high bits. */
#ifndef TEST_LAYOUT_PROTOTYPE_H
#define TEST_LAYOUT_PROTOTYPE_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <limits.h>

#define LP_VERSION 1
#define LP_CLIENTS 8
#define LP_HEADER 8
#define LP_RECORD 11
#define LP_WORDS (LP_HEADER + LP_CLIENTS * LP_RECORD)
#define LP_PREPARE_MS 16
#define LP_TTL_MS 1000
#define LP_PREPARE 1
#define LP_READY 2
#define LP_COMMIT 3
#define LP_CANCEL 4
#define LP_IDLE 0
#define LP_WAITING 1
#define LP_CAPTURED 2
#define LP_ACTIVE 3
#define LP_CAP_ATOM "_DANIWM_TEST_LAYOUT_CAP_V1"
#define LP_DATA_ATOM "_DANIWM_TEST_LAYOUT_DATA_V1"
#define LP_EVENT_ATOM "_DANIWM_TEST_LAYOUT_EVENT_V1"

typedef struct { int x, y, w, h; } LPRect;
typedef struct { uint32_t id; uint64_t generation; LPRect old, target; } LPClient;
typedef struct {
    uint32_t wm, compositor, epoch, sequence, duration, op, count;
    LPClient clients[LP_CLIENTS];
} LPBatch;
typedef struct {
    LPBatch batch;
    uint32_t latest;
    int phase;
    int64_t prepared, started;
} LPState;

static inline int lp_rect_valid(LPRect r) {
    /* Conservative X11 rectangle bounds, including translated extents. */
    return r.x >= -32768 && r.y >= -32768 && r.x <= 32767 && r.y <= 32767 &&
        r.w > 0 && r.h > 0 && r.w <= 32767 && r.h <= 32767 &&
        (int64_t)r.x + r.w <= 32767 && (int64_t)r.y + r.h <= 32767;
}
static inline int lp_signed(unsigned long word) {
    return word <= INT32_MAX ? (int)word : (int)((int64_t)word - INT64_C(4294967296));
}
static inline int lp_decode(LPBatch *out, const unsigned long *v, size_t n,
                           unsigned long extra, uint32_t wm, uint32_t cm, uint32_t epoch) {
    if (!out || !v || n < LP_HEADER || n > LP_WORDS || extra || !wm || !cm || !epoch) return 0;
    for (size_t i = 0; i < n; i++) if (v[i] > UINT32_MAX) return 0;
    if (v[0] != LP_VERSION || v[1] != wm || v[2] != cm || v[3] != epoch || !v[4] ||
        !v[5] || v[5] > LP_CLIENTS || v[6] > 1000 || v[7] < LP_PREPARE || v[7] > LP_CANCEL ||
        n != LP_HEADER + v[5] * LP_RECORD) return 0;
    LPBatch b = {0};
    b.wm = wm; b.compositor = cm; b.epoch = epoch; b.sequence = (uint32_t)v[4];
    b.count = (uint32_t)v[5]; b.duration = (uint32_t)v[6]; b.op = (uint32_t)v[7];
    for (unsigned i = 0; i < b.count; i++) {
        const unsigned long *p = v + LP_HEADER + i * LP_RECORD;
        LPClient *c = &b.clients[i];
        c->id = (uint32_t)p[0]; c->generation = (uint64_t)p[1] | ((uint64_t)p[2] << 32);
        c->old = (LPRect){lp_signed(p[3]), lp_signed(p[4]), lp_signed(p[5]), lp_signed(p[6])};
        c->target = (LPRect){lp_signed(p[7]), lp_signed(p[8]), lp_signed(p[9]), lp_signed(p[10])};
        if (!c->id || !c->generation || !lp_rect_valid(c->old) || !lp_rect_valid(c->target)) return 0;
        for (unsigned j = 0; j < i; j++) if (b.clients[j].id == c->id) return 0;
    }
    *out = b;
    return 1;
}
static inline size_t lp_encode(const LPBatch *b, unsigned long *v) {
    if (!b || !v || b->count > LP_CLIENTS) return 0;
    v[0] = LP_VERSION; v[1] = b->wm; v[2] = b->compositor; v[3] = b->epoch;
    v[4] = b->sequence; v[5] = b->count; v[6] = b->duration; v[7] = b->op;
    for (unsigned i = 0; i < b->count; i++) {
        const LPClient *c = &b->clients[i]; unsigned long *p = v + LP_HEADER + i * LP_RECORD;
        p[0] = c->id; p[1] = (uint32_t)c->generation; p[2] = (uint32_t)(c->generation >> 32);
        int r[] = {c->old.x,c->old.y,c->old.w,c->old.h,c->target.x,c->target.y,c->target.w,c->target.h};
        for (unsigned j = 0; j < 8; j++) p[3+j] = (uint32_t)r[j];
    }
    return LP_HEADER + b->count * LP_RECORD;
}
static inline int lp_newer(uint32_t a, uint32_t b) {
    return a && (!b || ((uint32_t)(a-b) && (uint32_t)(a-b) <= INT32_MAX));
}
static inline void lp_cancel(LPState *s) { s->phase = LP_IDLE; }
static inline void lp_tick(LPState *s, int64_t now) {
    if (s->phase == LP_IDLE) return;
    if (now < s->prepared || now - s->prepared >= LP_TTL_MS ||
        ((s->phase == LP_WAITING || s->phase == LP_CAPTURED) && now - s->prepared >= LP_PREPARE_MS) ||
        (s->phase == LP_ACTIVE && (now < s->started || now - s->started >= s->batch.duration))) lp_cancel(s);
}
static inline int lp_begin(LPState *s, const LPBatch *b, int64_t now) {
    if (!b || b->op != LP_PREPARE || now < 0 || !lp_newer(b->sequence,s->latest)) return 0;
    s->latest = b->sequence; s->batch = *b; s->prepared = now; s->phase = LP_WAITING;
    if (!b->duration) { lp_cancel(s); return 0; }
    return 1;
}
static inline int lp_matching(const LPState *s, const LPBatch *b) {
    LPBatch expected = s->batch, received = *b;
    expected.op = received.op = 0;
    /* Both are initialized/decoded structures; compare fields, not padding. */
    if (expected.wm != received.wm || expected.compositor != received.compositor ||
        expected.epoch != received.epoch || expected.sequence != received.sequence ||
        expected.duration != received.duration || expected.count != received.count) return 0;
    for (unsigned i=0;i<expected.count;i++) {
        const LPClient *a=&expected.clients[i], *c=&received.clients[i];
        if (a->id!=c->id || a->generation!=c->generation ||
            a->old.x!=c->old.x || a->old.y!=c->old.y || a->old.w!=c->old.w || a->old.h!=c->old.h ||
            a->target.x!=c->target.x || a->target.y!=c->target.y ||
            a->target.w!=c->target.w || a->target.h!=c->target.h) return 0;
    }
    return 1;
}
static inline int lp_ready(LPState *s, const LPBatch *b, int64_t now) {
    lp_tick(s,now);
    if (s->phase!=LP_WAITING || b->op!=LP_READY || !lp_matching(s,b)) return 0;
    s->phase=LP_CAPTURED; return 1;
}
static inline int lp_commit(LPState *s, const LPBatch *b, int64_t now) {
    lp_tick(s,now);
    if (b->op!=LP_COMMIT || !lp_matching(s,b)) return 0;
    if (s->phase!=LP_CAPTURED) { lp_cancel(s); return 0; }
    s->started=now; s->phase=LP_ACTIVE; return 1;
}
static inline double lp_ease(double t) { double rest=1.0-t; return 1.0-rest*rest*rest; }
static inline LPRect lp_sample(LPClient c, int64_t start, unsigned duration, int64_t now) {
    if (!duration || now < start || (uint64_t)(now-start) >= duration) return c.target;
    double t=lp_ease((double)(now-start)/duration);
    return (LPRect){c.old.x+(int)((c.target.x-c.old.x)*t), c.old.y+(int)((c.target.y-c.old.y)*t),
        c.old.w+(int)((c.target.w-c.old.w)*t), c.old.h+(int)((c.target.h-c.old.h)*t)};
}
#endif
