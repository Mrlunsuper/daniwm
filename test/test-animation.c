/* Phase 0: deterministic ownership/budget tests. No X connection. */
#include "animation-snapshot.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>

typedef struct {
    unsigned captures, pixmaps, pictures, releases, fail;
    unsigned long serial;
} Fake;

static int capture(void *context, SnapshotKey key, unsigned w, unsigned h,
                   unsigned depth, SnapshotImage *image) {
    Fake *f = context;
    (void)key; (void)w; (void)h; (void)depth;
    f->captures++;
    if (f->fail == 1) return 0;
    image->pixmap = ++f->serial;
    f->pixmaps++;
    if (f->fail == 2) return 0;
    image->picture = ++f->serial;
    f->pictures++;
    return f->fail != 3;
}

static void release(void *context, SnapshotImage image) {
    Fake *f = context;
    if (image.picture) { assert(f->pictures); f->pictures--; }
    if (image.pixmap) { assert(f->pixmaps); f->pixmaps--; }
    f->releases++;
}

int main(void) {
    size_t bytes = 0;
    assert(snapshot_size(1, 2, 24, 32, &bytes) && bytes == 8);
    assert(snapshot_size(1920, 1080, 32, 32, &bytes) && bytes == 8294400);
    assert(!snapshot_size(0, 1, 32, 32, &bytes));
    assert(!snapshot_size(1, 0, 32, 32, &bytes));
    assert(!snapshot_size(1, 1, 0, 32, &bytes));
    assert(!snapshot_size(1, 1, 128, 32, &bytes));
    assert(!snapshot_size(1, 1, 32, 7, &bytes));
    assert(!snapshot_size(UINT_MAX, UINT_MAX, 64, 32, &bytes));
    puts("PASS: padded stride, zero dimensions and size overflow validation");

    SnapshotPool pool;
    Fake f = {0};
    snapshot_init(&pool, capture, release, &f);
    SnapshotKey old = {42, 1}, next = {42, 2};
    assert(!snapshot_take(&pool, (SnapshotKey){42, 0}, 1, 1, 24, 32, 32, 0));
    assert(!snapshot_take(&pool, old, 1, 1, 32, 24, 32, 0));
    assert(!snapshot_take(&pool, old, UINT_MAX, UINT_MAX, 32, 32, 32, 0));
    assert(!snapshot_take(&pool, old, 1, 1, 24, 32, 32, -1));
    assert(f.captures == 0);
    assert(snapshot_take(&pool, old, 100, 100, 24, 32, 32, 0));
    assert(!snapshot_get(&pool, next, 0));
    snapshot_drop(&pool, next);
    assert(pool.count == 1);
    assert(snapshot_take(&pool, next, 100, 100, 24, 32, 32, 0));
    snapshot_drop(&pool, old);
    assert(!snapshot_get(&pool, old, 0) && snapshot_get(&pool, next, 0));
    assert(pool.count == 1);
    snapshot_clear(&pool);
    assert(!pool.count && !pool.bytes && !f.pixmaps && !f.pictures);
    puts("PASS: generation-specific lookup/cancellation isolates reused identities");

    for (unsigned i = 0; i < SNAPSHOT_COUNT; i++)
        assert(snapshot_take(&pool, (SnapshotKey){i + 1, 1}, 1, 1, 24, 32, 32, 0));
    unsigned calls = f.captures;
    assert(!snapshot_take(&pool, old, 1, 1, 24, 32, 32, 0));
    assert(f.captures == calls && pool.count == SNAPSHOT_COUNT);
    snapshot_clear(&pool);
    assert(snapshot_take(&pool, old, 4096, 2048, 32, 32, 32, 0));
    assert(pool.bytes == SNAPSHOT_BYTES);
    calls = f.captures;
    assert(!snapshot_take(&pool, next, 1, 1, 24, 32, 32, 0));
    assert(f.captures == calls);
    /* Replace releases first: no hidden old+new allocation over budget. */
    assert(snapshot_take(&pool, old, 4096, 2048, 32, 32, 32, 1));
    assert(pool.count == 1 && f.pixmaps == 1 && f.pictures == 1);
    assert(snapshot_get(&pool, old, SNAPSHOT_TTL_MS));
    assert(!snapshot_get(&pool, old, SNAPSHOT_TTL_MS + 1));
    assert(!pool.count && !f.pixmaps && !f.pictures);
    assert(snapshot_take(&pool, old, 1, 1, 24, 32, 32, 10));
    snapshot_expire(&pool, 9);
    assert(!pool.count); /* malformed backwards time fails closed */
    puts("PASS: count/byte admission before capture, exact TTL and cancellation");

    for (unsigned failure = 1; failure <= 3; failure++) {
        f.fail = failure;
        assert(!snapshot_take(&pool, old, 1, 1, 24, 32, 32, 0));
        assert(!pool.count && !pool.bytes && !f.pixmaps && !f.pictures);
    }
    f.fail = 0;
    for (unsigned i = 0; i < 10000; i++) {
        SnapshotKey key = {42, i + 1};
        assert(snapshot_take(&pool, key, 200, 100, 24, 32, 32, i));
        snapshot_drop(&pool, key); /* completion/unmap/destroy/cancel */
        snapshot_drop(&pool, key);
        assert(!pool.count && !pool.bytes && !f.pixmaps && !f.pictures);
    }
    snapshot_clear(&pool);
    printf("PASS: partial allocation failures and 10000 lifecycle cycles; "
           "peak_count=%u peak_bytes=%zu outstanding_images=%u/%u\n",
           pool.peak_count, pool.peak_bytes, f.pixmaps, f.pictures);
    return 0;
}
