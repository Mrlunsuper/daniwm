/* Bounded test-only retained image pool; no rendering or WM integration. */
#include "animation-snapshot.h"
#include <string.h>

static int same_key(SnapshotKey a, SnapshotKey b) {
    return a.identity == b.identity && a.generation == b.generation;
}

int snapshot_size(unsigned width, unsigned height, unsigned bpp,
                  unsigned scanline_pad, size_t *bytes) {
    if (!bytes || !width || !height || !bpp || bpp > 64 ||
        (scanline_pad != 8 && scanline_pad != 16 && scanline_pad != 32)) return 0;
    if ((size_t)width > (SIZE_MAX - (scanline_pad - 1)) / bpp) return 0;
    size_t bits = (size_t)width * bpp;
    size_t stride = ((bits + scanline_pad - 1) / scanline_pad) * (scanline_pad / 8);
    if (stride > SIZE_MAX / height) return 0;
    *bytes = stride * height;
    return 1;
}

void snapshot_init(SnapshotPool *pool, SnapshotCapture capture,
                   SnapshotRelease release, void *context) {
    memset(pool, 0, sizeof(*pool));
    pool->capture = capture;
    pool->release = release;
    pool->context = context;
}

static void release_slot(SnapshotPool *pool, Snapshot *slot) {
    if (!slot->bytes) return;
    pool->release(pool->context, slot->image);
    pool->bytes -= slot->bytes;
    pool->count--;
    memset(slot, 0, sizeof(*slot));
}

void snapshot_expire(SnapshotPool *pool, int64_t now) {
    for (unsigned i = 0; i < SNAPSHOT_COUNT; i++) {
        Snapshot *s = &pool->slots[i];
        /* Backwards time is invalid; cancel instead of extending lifetime. */
        if (s->bytes && (now < s->captured_ms ||
            now - s->captured_ms >= SNAPSHOT_TTL_MS)) release_slot(pool, s);
    }
}

const Snapshot *snapshot_get(SnapshotPool *pool, SnapshotKey key, int64_t now) {
    snapshot_expire(pool, now);
    for (unsigned i = 0; i < SNAPSHOT_COUNT; i++)
        if (pool->slots[i].bytes && same_key(pool->slots[i].key, key))
            return &pool->slots[i];
    return NULL;
}

void snapshot_drop(SnapshotPool *pool, SnapshotKey key) {
    for (unsigned i = 0; i < SNAPSHOT_COUNT; i++)
        if (same_key(pool->slots[i].key, key)) release_slot(pool, &pool->slots[i]);
}

void snapshot_clear(SnapshotPool *pool) {
    for (unsigned i = 0; i < SNAPSHOT_COUNT; i++) release_slot(pool, &pool->slots[i]);
}

int snapshot_take(SnapshotPool *pool, SnapshotKey key, unsigned width,
                  unsigned height, unsigned depth, unsigned bpp,
                  unsigned scanline_pad, int64_t now) {
    size_t bytes;
    if (!key.identity || !key.generation || now < 0 || !depth || depth > bpp ||
        !pool->capture || !pool->release ||
        !snapshot_size(width, height, bpp, scanline_pad, &bytes) ||
        bytes > SNAPSHOT_BYTES) return 0;
    snapshot_expire(pool, now);
    snapshot_drop(pool, key);
    /* Admission includes the in-flight image: never allocate beyond budget. */
    if (pool->count == SNAPSHOT_COUNT || bytes > SNAPSHOT_BYTES - pool->bytes)
        return 0;
    SnapshotImage image = {0};
    if (!pool->capture(pool->context, key, width, height, depth, &image) ||
        !image.pixmap || !image.picture) {
        pool->release(pool->context, image);
        return 0;
    }
    for (unsigned i = 0; i < SNAPSHOT_COUNT; i++) {
        if (pool->slots[i].bytes) continue;
        pool->slots[i] = (Snapshot){key, image, bytes, now, width, height, depth};
        pool->bytes += bytes;
        pool->count++;
        if (pool->bytes > pool->peak_bytes) pool->peak_bytes = pool->bytes;
        if (pool->count > pool->peak_count) pool->peak_count = pool->count;
        return 1;
    }
    pool->release(pool->context, image);
    return 0;
}
