/* Phase 3-P1 headless checks: transaction codec, ordering, budget and scene state. */
#include "ws-capture-prototype.h"
#include <assert.h>
#include <stdio.h>

static unsigned released;
static void release(unsigned long pixmap) { assert(pixmap); released++; }

static WSTxn txn(uint32_t sequence, uint32_t count) {
    WSTxn t = {0};
    t.wm = 10; t.compositor = 20; t.epoch = 3; t.sequence = sequence;
    t.from = 0; t.to = 1; t.sent_ms = 1000; t.deadline_ms = 100; t.count = count;
    for (uint32_t i = 0; i < count; i++) t.members[i] = 100 + i;
    return t;
}
int main(void) {
    unsigned long v[WSP_WORDS + 2];
    WSTxn in = txn(7, 3), out;
    size_t n = wsp_encode(v, &in);
    assert(n == WSP_HEADER + 3);
    assert(wsp_decode(&out, v, n, 0, 10, 20, 3));
    assert(out.sequence == 7 && out.count == 3 && out.members[2] == 102);
    /* Wrong owner, compositor, epoch, extra data and truncated payloads fail. */
    assert(!wsp_decode(&out, v, n, 0, 11, 20, 3));
    assert(!wsp_decode(&out, v, n, 0, 10, 21, 3));
    assert(!wsp_decode(&out, v, n, 0, 10, 20, 4));
    assert(!wsp_decode(&out, v, n, 1, 10, 20, 3));
    assert(!wsp_decode(&out, v, n - 1, 0, 10, 20, 3));
    assert(!wsp_decode(&out, v, WSP_HEADER - 1, 0, 10, 20, 3));
    unsigned long bad[WSP_WORDS];
    for (unsigned field = 0; field < 10; field++) {
        size_t m = wsp_encode(bad, &in);
        if (field == 0) bad[0] = 2;
        if (field == 1) bad[4] = 0;              /* zero sequence */
        if (field == 2) bad[5] = bad[6];         /* from == to */
        if (field == 3) bad[8] = 0;              /* zero deadline */
        if (field == 4) bad[8] = WSP_MAX_DEADLINE_MS + 1;
        if (field == 5) bad[9] = WSP_MEMBERS + 1;
        if (field == 6) bad[WSP_HEADER] = 0;     /* zero member */
        if (field == 7) bad[WSP_HEADER + 1] = bad[WSP_HEADER]; /* duplicate */
        if (field == 8) bad[4] = (unsigned long)UINT32_MAX + 1; /* non-wire bits */
        if (field == 9) bad[9] = 2;              /* count does not match length */
        assert(!wsp_decode(&out, bad, m, 0, 10, 20, 3));
    }
    WSTxn empty = txn(8, 0);
    n = wsp_encode(v, &empty);
    assert(wsp_decode(&out, v, n, 0, 10, 20, 3) && out.count == 0);

    /* Sequence order with wrap, and age with clock wrap. */
    assert(wsp_newer(1, 0) && wsp_newer(2, 1) && !wsp_newer(1, 1) && !wsp_newer(1, 2));
    assert(wsp_newer(1, UINT32_MAX) && !wsp_newer(0, 5));
    assert(wsp_age_ok(1000, 1100, 100) && !wsp_age_ok(1000, 1101, 100));
    assert(wsp_age_ok(UINT32_MAX - 10, 20, 100) && !wsp_age_ok(UINT32_MAX - 200, 20, 100));

    /* Image bytes use the padded stride. */
    uint64_t bytes;
    assert(wsp_image_bytes(&bytes, 100, 80, 32, 32) && bytes == 100u * 4u * 80u);
    assert(wsp_image_bytes(&bytes, 3, 2, 24, 32) && bytes == 12u * 2u);
    assert(!wsp_image_bytes(&bytes, 0, 1, 32, 32) && !wsp_image_bytes(&bytes, 1, 1, 32, 7));

    /* Budget admission happens before any retention. */
    WSScene scene = {0};
    WSTxn two = txn(9, 2);
    uint64_t gens[WSP_MEMBERS] = {5, 6};
    uint64_t fits[WSP_MEMBERS] = {WSP_BUDGET / 2, WSP_BUDGET / 2};
    uint64_t over[WSP_MEMBERS] = {WSP_BUDGET / 2, WSP_BUDGET / 2 + 1};
    uint64_t huge[WSP_MEMBERS] = {UINT64_MAX, 1};
    assert(!wsp_begin(&scene, &two, over, gens, 0) && scene.phase == WSP_IDLE);
    assert(!wsp_begin(&scene, &two, huge, gens, 0) && scene.phase == WSP_IDLE);
    assert(wsp_begin(&scene, &two, fits, gens, 0) && scene.phase == WSP_COLLECTING);

    /* Retention requires a member, its PREPARE generation and one image each. */
    assert(wsp_member(&scene, 999) < 0);
    int a = wsp_member(&scene, 100), b = wsp_member(&scene, 101);
    assert(a == 0 && b == 1);
    assert(!wsp_retain(&scene, a, 0, 5));
    assert(!wsp_retain(&scene, a, 77, 6));      /* generation changed (remap) */
    assert(wsp_retain(&scene, a, 77, 5));
    assert(!wsp_retain(&scene, a, 78, 5));      /* no second image */
    assert(scene.phase == WSP_COLLECTING);
    assert(wsp_retain(&scene, b, 88, 6) && scene.phase == WSP_READY);
    assert(!wsp_retain(&scene, b, 89, 6));      /* ready scene is frozen */
    assert(!wsp_expired(&scene, WSP_TTL_MS) && wsp_expired(&scene, WSP_TTL_MS + 1));
    released = 0;
    wsp_clear(&scene, release);
    assert(released == 2 && scene.phase == WSP_IDLE && wsp_member(&scene, 100) < 0);

    /* Stacking order: members sort bottom to top by recorded rank. */
    WSTxn three = txn(10, 3);
    uint64_t small[WSP_MEMBERS] = {1, 1, 1}, g3[WSP_MEMBERS] = {1, 1, 1};
    assert(wsp_begin(&scene, &three, small, g3, 0));
    scene.m[0].rank = 7; scene.m[1].rank = 2; scene.m[2].rank = 5;
    unsigned order[WSP_MEMBERS];
    wsp_stack_order(&scene, order);
    assert(order[0] == 1 && order[1] == 2 && order[2] == 0);
    wsp_clear(&scene, release);

    /* An empty outgoing workspace is ready at once and owns nothing. */
    uint64_t none[WSP_MEMBERS] = {0};
    assert(wsp_begin(&scene, &empty, none, none, 0) && scene.phase == WSP_READY);
    released = 0; wsp_clear(&scene, release); assert(released == 0);

    /* 2000 begin/retain/clear cycles release every image exactly once. */
    released = 0;
    for (unsigned i = 0; i < 2000; i++) {
        assert(wsp_begin(&scene, &two, fits, gens, i));
        assert(wsp_retain(&scene, 0, 1000 + i, 5));
        if (i % 2) assert(wsp_retain(&scene, 1, 5000 + i, 6));
        wsp_clear(&scene, release);
    }
    assert(released == 3000);
    puts("PASS: 3-P1 codec bounds, owner/epoch/sequence/age checks, padded budget admission, "
         "generation-bound retention, stacking order and 2000 scene cycles");
    return 0;
}
