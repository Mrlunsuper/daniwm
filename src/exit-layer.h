/* Detached visual resources only: no live client pointers or X queries. */
#ifndef DANIWM_EXIT_LAYER_H
#define DANIWM_EXIT_LAYER_H
#include "animation.h"
#include <stddef.h>
#include <stdint.h>
#define EXIT_COUNT 8
#define EXIT_BYTES ((size_t)32 * 1024 * 1024)
#define EXIT_TTL_MS 1000

typedef struct {
    unsigned long identity, pixmap, picture;
    uint64_t generation;
    size_t bytes;
    long long retained;
    int x, y, width, height;
    unsigned rank;
    unsigned long above;
    uint64_t above_generation;
    struct { int16_t x, y; uint16_t width, height; } clip[256];
    unsigned nclip;
    int shaped;
    int workspace; /* part of a workspace crossfade, not a close */
    double alpha, dim;
    Animation animation;
} ExitLayer;
typedef struct {
    ExitLayer layers[EXIT_COUNT];
    unsigned count;
    size_t bytes;
} ExitPool;
typedef void (*ExitRelease)(unsigned long picture, unsigned long pixmap);
int exit_size(unsigned width, unsigned height, unsigned bpp, unsigned pad, size_t *bytes);
int exit_admit(ExitPool *pool, const ExitLayer *layer);
void exit_drop(ExitPool *pool, unsigned index, ExitRelease release);
void exit_clear(ExitPool *pool, ExitRelease release);
void exit_cancel_identity(ExitPool *pool, unsigned long identity, ExitRelease release);
void exit_expire(ExitPool *pool, long long now, ExitRelease release);
#endif
