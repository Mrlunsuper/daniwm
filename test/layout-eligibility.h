/* Phase 2 design experiment ONLY; not linked into either executable.
 * Sampling pointer/cover/input-shape state cannot certify future delivery.
 * There is intentionally NO animation-eligible result under this contract. */
#ifndef TEST_LAYOUT_ELIGIBILITY_H
#define TEST_LAYOUT_ELIGIBILITY_H
#include "layout-prototype.h"

typedef enum {
    LE_INVALID,
    LE_DISABLED,
    LE_NO_CHANGE,
    LE_UNPROVEN_INPUT_GEOMETRY
} LEligibility;
typedef struct {
    int compositor, enabled, reduced;
    /* Observations deliberately cannot override the safety decision. */
    int pointer_outside, input_transparent_visual, covered, input_shape_empty;
} LEObservation;
static inline LEligibility le_decide(const LPBatch *b, LEObservation o) {
    if (!b || !b->wm || !b->compositor || !b->epoch || !b->sequence ||
        !b->count || b->count > LP_CLIENTS || b->duration > 1000 || b->op != LP_PREPARE)
        return LE_INVALID;
    for (unsigned i=0;i<b->count;i++) {
        const LPClient *c=&b->clients[i];
        if (!c->id || !c->generation || !lp_rect_valid(c->old) || !lp_rect_valid(c->target))
            return LE_INVALID;
        for (unsigned j=0;j<i;j++) if (b->clients[j].id==c->id) return LE_INVALID;
    }
    if (!o.compositor || !o.enabled || o.reduced || !b->duration) return LE_DISABLED;
    for (unsigned i=0;i<b->count;i++) {
        const LPClient *c=&b->clients[i];
        if (c->old.x!=c->target.x || c->old.y!=c->target.y ||
            c->old.w!=c->target.w || c->old.h!=c->target.h) return LE_UNPROVEN_INPUT_GEOMETRY;
    }
    /* A no-op is instant, NOT an animation or a Phase 2 exit-gate pass. */
    return LE_NO_CHANGE;
}
#endif
