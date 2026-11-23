/* Headless Phase 2 prototype, deliberately not a supported wire contract. */
#include "layout-prototype.h"
#include <assert.h>
#include <stdio.h>
static LPBatch example(void) {
    LPBatch b={.wm=10,.compositor=20,.epoch=30,.sequence=1,.duration=160,.op=LP_PREPARE,.count=2};
    b.clients[0]=(LPClient){40,UINT64_C(0x100000002),{-10,10,200,100},{240,20,120,200}};
    b.clients[1]=(LPClient){41,3,{240,20,120,200},{-10,10,200,100}};
    return b;
}
int main(void) {
    LPBatch b=example(), decoded; unsigned long v[LP_WORDS+1];
    size_t n=lp_encode(&b,v); assert(n==LP_HEADER+2*LP_RECORD);
    assert(lp_decode(&decoded,v,n,0,10,20,30) && lp_matching(&(LPState){.batch=b},&decoded));
    assert(!lp_decode(&decoded,v,n,1,10,20,30));
    assert(!lp_decode(&decoded,v,n,0,11,20,30));
    assert(!lp_decode(&decoded,v,n,0,10,21,30));
    assert(!lp_decode(&decoded,v,n,0,10,20,31));
    for (size_t i=0;i<n;i++) assert(!lp_decode(&decoded,v,i,0,10,20,30));
    assert(!lp_decode(&decoded,v,n+1,0,10,20,30));
    unsigned long original=v[5]; v[5]=LP_CLIENTS+1; assert(!lp_decode(&decoded,v,n,0,10,20,30)); v[5]=original;
    v[LP_HEADER+LP_RECORD]=40; assert(!lp_decode(&decoded,v,n,0,10,20,30)); lp_encode(&b,v);
    v[LP_HEADER+5]=0; assert(!lp_decode(&decoded,v,n,0,10,20,30)); lp_encode(&b,v);
    v[6]=1001; assert(!lp_decode(&decoded,v,n,0,10,20,30)); lp_encode(&b,v);
#if ULONG_MAX > UINT32_MAX
    v[4]=(unsigned long)UINT32_MAX+1; assert(!lp_decode(&decoded,v,n,0,10,20,30)); lp_encode(&b,v);
#endif
    for (unsigned i=0;i<2000;i++) {
        LPState s={0}; b=example(); assert(lp_begin(&s,&b,100));
        LPBatch msg=b; msg.op=LP_READY; msg.clients[0].generation++;
        assert(!lp_ready(&s,&msg,101)); msg=b; msg.op=LP_READY;
        assert(lp_ready(&s,&msg,101)); msg.op=LP_COMMIT;
        assert(lp_commit(&s,&msg,102) && s.phase==LP_ACTIVE);
        lp_tick(&s,261); assert(s.phase==LP_ACTIVE); lp_tick(&s,262); assert(s.phase==LP_IDLE);
        assert(!lp_begin(&s,&b,300)); b.sequence++;
        assert(lp_begin(&s,&b,300)); msg=b; msg.op=LP_COMMIT;
        assert(!lp_commit(&s,&msg,300) && s.phase==LP_IDLE);
        msg.op=LP_READY; assert(!lp_ready(&s,&msg,301)); /* late READY never revives */
        b.sequence++; assert(lp_begin(&s,&b,400)); lp_tick(&s,400+LP_PREPARE_MS); assert(s.phase==LP_IDLE);
        b.sequence++; assert(lp_begin(&s,&b,500)); lp_tick(&s,499); assert(s.phase==LP_IDLE);
        b.sequence++; assert(lp_begin(&s,&b,600)); lp_cancel(&s); lp_cancel(&s); assert(s.phase==LP_IDLE);
    }
    assert(lp_newer(1,UINT32_MAX) && !lp_newer(UINT32_MAX,1));
    b=example(); LPClient c=b.clients[0];
    LPRect r=lp_sample(c,100,160,100); assert(r.x==c.old.x && r.w==c.old.w);
    r=lp_sample(c,100,160,260); assert(r.x==c.target.x && r.w==c.target.w);
    r=lp_sample(c,100,0,100); assert(r.x==c.target.x);
    for(int t=0;t<=160;t++) {
        r=lp_sample(c,100,160,100+t); assert(lp_rect_valid(r));
        assert(r.x>=c.old.x && r.x<=c.target.x);
    }
    r=lp_sample(c,100,160,140); c.old=r; c.target=(LPRect){-30,30,80,90};
    LPRect first=lp_sample(c,140,160,140); assert(first.x==r.x && first.w==r.w);
    r=lp_sample(c,140,160,10000); assert(r.x==c.target.x && r.w==c.target.w);
    puts("PASS: test-only bounded batch decode, owner/epoch/generation, stale/late rejection, 2000 lifecycle cycles, cubic rectangles/retarget/zero/long gap");
}
