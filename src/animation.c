#include "animation.h"

void animation_cancel(Animation *a) { a->active = 0; }

void animation_start(Animation *a, uint64_t identity, uint64_t generation,
                     int64_t now_ms, unsigned duration_ms) {
    *a = (Animation){identity, generation, now_ms, duration_ms,
        identity && generation && now_ms >= 0 && duration_ms > 0};
}

double animation_value(const Animation *a, uint64_t identity, uint64_t generation,
                       int64_t now_ms) {
    if (!a->active || a->identity != identity || a->generation != generation ||
        !a->duration_ms || now_ms < a->start_ms) return 1.0;
    uint64_t elapsed = (uint64_t)now_ms - (uint64_t)a->start_ms;
    if (elapsed >= a->duration_ms) return 1.0;
    return (double)elapsed / a->duration_ms;
}

double animation_sample(Animation *a, uint64_t identity, uint64_t generation,
                        int64_t now_ms) {
    double value = animation_value(a, identity, generation, now_ms);
    if (value >= 1.0) animation_cancel(a);
    return value;
}
