/* Phase 1A: deterministic linear opening fade; caller supplies monotonic ms. */
#ifndef DANIWM_ANIMATION_H
#define DANIWM_ANIMATION_H
#include <stdint.h>

typedef struct {
    uint64_t identity, generation;
    int64_t start_ms;
    unsigned duration_ms;
    int active;
} Animation;

void animation_start(Animation *a, uint64_t identity, uint64_t generation,
                     int64_t now_ms, unsigned duration_ms);
void animation_cancel(Animation *a);
double animation_value(const Animation *a, uint64_t identity, uint64_t generation,
                       int64_t now_ms);
double animation_sample(Animation *a, uint64_t identity, uint64_t generation,
                        int64_t now_ms);
#endif
