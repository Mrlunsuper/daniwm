#include "layout-eligibility.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    LPBatch b={.wm=1,.compositor=2,.epoch=3,.sequence=1,.duration=160,.op=LP_PREPARE,.count=1};
    b.clients[0]=(LPClient){4,5,{20,20,100,80},{220,20,120,90}};
    unsigned rejected=0;
    /* Every combination of sampled pointer, cover and shape observations is
     * still only a snapshot. None can certify future event delivery. */
    for (unsigned flags=0;flags<16;flags++) {
        LEObservation o={.compositor=1,.enabled=1,.pointer_outside=!!(flags&1),
            .input_transparent_visual=!!(flags&2),.covered=!!(flags&4),.input_shape_empty=!!(flags&8)};
        assert(le_decide(&b,o)==LE_UNPROVEN_INPUT_GEOMETRY); rejected++;
        LPBatch pure_move=b; pure_move.clients[0].target.w=100; pure_move.clients[0].target.h=80;
        assert(le_decide(&pure_move,o)==LE_UNPROVEN_INPUT_GEOMETRY); rejected++;
        LPBatch pure_resize=b; pure_resize.clients[0].target.x=20;
        assert(le_decide(&pure_resize,o)==LE_UNPROVEN_INPUT_GEOMETRY); rejected++;
        o.enabled=0; assert(le_decide(&b,o)==LE_DISABLED); o.enabled=1;
        o.compositor=0; assert(le_decide(&b,o)==LE_DISABLED); o.compositor=1;
        o.reduced=1; assert(le_decide(&b,o)==LE_DISABLED); o.reduced=0;
        b.duration=0; assert(le_decide(&b,o)==LE_DISABLED); b.duration=160;
        LPBatch no_op=b; no_op.clients[0].target=no_op.clients[0].old;
        assert(le_decide(&no_op,o)==LE_NO_CHANGE);
    }
    LEObservation o={.compositor=1,.enabled=1};
    assert(le_decide(NULL,o)==LE_INVALID);
    b.clients[0].generation=0; assert(le_decide(&b,o)==LE_INVALID); b.clients[0].generation=5;
    b.count=LP_CLIENTS+1; assert(le_decide(&b,o)==LE_INVALID); b.count=1;
    b.duration=1001; assert(le_decide(&b,o)==LE_INVALID); b.duration=160;
    b.clients[0].target.w=0; assert(le_decide(&b,o)==LE_INVALID);
    printf("PASS: fail-closed eligibility rejects %u moving/resizing observation combinations; no-op/disabled are instant, not eligible\n",rejected);
    return 0;
}
