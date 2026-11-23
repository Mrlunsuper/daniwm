#include "exit-layer.h"
#include <string.h>

int exit_size(unsigned width, unsigned height, unsigned bpp, unsigned pad, size_t *bytes) {
    if (!bytes || !width || !height || !bpp || bpp > 64 ||
        (pad != 8 && pad != 16 && pad != 32)) return 0;
    if ((size_t)width > (SIZE_MAX - (pad - 1)) / bpp) return 0;
    size_t stride = (((size_t)width * bpp + pad - 1) / pad) * (pad / 8);
    if (stride > SIZE_MAX / height) return 0;
    *bytes = stride * height;
    return 1;
}
int exit_admit(ExitPool *pool, const ExitLayer *layer) {
    if (!layer->identity || !layer->generation || !layer->pixmap || !layer->picture ||
        !layer->bytes || layer->bytes > EXIT_BYTES || pool->count >= EXIT_COUNT ||
        layer->bytes > EXIT_BYTES - pool->bytes) return 0;
    for (unsigned i = 0; i < EXIT_COUNT; i++) if (!pool->layers[i].bytes) {
        pool->layers[i] = *layer;
        pool->count++;
        pool->bytes += layer->bytes;
        return 1;
    }
    return 0;
}
void exit_drop(ExitPool *pool, unsigned index, ExitRelease release) {
    if (index >= EXIT_COUNT || !pool->layers[index].bytes) return;
    ExitLayer *layer = &pool->layers[index];
    /* Unpublish before issuing asynchronous resource releases. */
    ExitLayer old = *layer;
    memset(layer, 0, sizeof(*layer));
    pool->count--;
    pool->bytes -= old.bytes;
    release(old.picture, old.pixmap);
}
void exit_clear(ExitPool *pool, ExitRelease release) {
    for (unsigned i = 0; i < EXIT_COUNT; i++) exit_drop(pool, i, release);
}
void exit_cancel_identity(ExitPool *pool, unsigned long identity, ExitRelease release) {
    for (unsigned i = 0; i < EXIT_COUNT; i++)
        if (pool->layers[i].identity == identity) exit_drop(pool, i, release);
}
void exit_expire(ExitPool *pool, long long now, ExitRelease release) {
    for (unsigned i = 0; i < EXIT_COUNT; i++) {
        ExitLayer *layer = &pool->layers[i];
        if (layer->bytes && (now < layer->retained || now - layer->retained >= EXIT_TTL_MS ||
            animation_value(&layer->animation, layer->identity, layer->generation, now) >= 1.0))
            exit_drop(pool, i, release);
    }
}
