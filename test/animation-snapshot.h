/* Phase 0 experiment only. Not linked into daniwm or dani-comp. */
#ifndef TEST_ANIMATION_SNAPSHOT_H
#define TEST_ANIMATION_SNAPSHOT_H
#include <stddef.h>
#include <stdint.h>

#define SNAPSHOT_COUNT 8
#define SNAPSHOT_BYTES ((size_t)32 * 1024 * 1024)
#define SNAPSHOT_TTL_MS 1000

typedef struct { uint64_t identity, generation; } SnapshotKey;
typedef struct { unsigned long pixmap, picture; } SnapshotImage;
typedef struct {
    SnapshotKey key;
    SnapshotImage image;
    size_t bytes;
    int64_t captured_ms;
    unsigned width, height, depth;
} Snapshot;
/* Capture may fail with partially created resources. It must publish only
 * confirmed-valid handles; the pool releases partial results on failure. */
typedef int (*SnapshotCapture)(void *, SnapshotKey, unsigned, unsigned,
                              unsigned, SnapshotImage *);
typedef void (*SnapshotRelease)(void *, SnapshotImage);
typedef struct {
    Snapshot slots[SNAPSHOT_COUNT];
    size_t bytes, peak_bytes;
    unsigned count, peak_count;
    SnapshotCapture capture;
    SnapshotRelease release;
    void *context;
} SnapshotPool;

int snapshot_size(unsigned width, unsigned height, unsigned bpp,
                  unsigned scanline_pad, size_t *bytes);
void snapshot_init(SnapshotPool *pool, SnapshotCapture capture,
                   SnapshotRelease release, void *context);
void snapshot_expire(SnapshotPool *pool, int64_t now);
const Snapshot *snapshot_get(SnapshotPool *pool, SnapshotKey key, int64_t now);
int snapshot_take(SnapshotPool *pool, SnapshotKey key, unsigned width,
                  unsigned height, unsigned depth, unsigned bpp,
                  unsigned scanline_pad, int64_t now);
void snapshot_drop(SnapshotPool *pool, SnapshotKey key);
void snapshot_clear(SnapshotPool *pool);
#endif
