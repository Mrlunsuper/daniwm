#include "../src/exit-layer.h"
#include "../src/animation-lifecycle.h"
#include <assert.h>
#include <stdio.h>
static unsigned released;
static void release(unsigned long picture, unsigned long pixmap) {
    assert(picture && pixmap); released++;
}
int main(void) {
    ExitPool pool = {0};
    size_t bytes;
    assert(exit_size(3, 2, 24, 32, &bytes) && bytes == 24);
    assert(!exit_size(0, 2, 32, 32, &bytes));
    assert(!exit_size(1, 1, 32, 7, &bytes));
    ExitLayer s = {.identity=1, .generation=1, .pixmap=2, .picture=3,
        .bytes=EXIT_BYTES, .retained=100};
    animation_start(&s.animation, 1, 1, 100, 1000);
    assert(exit_admit(&pool, &s));
    assert(!exit_admit(&pool, &s));
    exit_expire(&pool, 1099, release); assert(pool.count == 1);
    exit_expire(&pool, 1100, release); assert(!pool.count && !pool.bytes && released == 1);
    s.bytes = 4;
    for (unsigned i=0; i<EXIT_COUNT; i++) { s.generation=i+1; assert(exit_admit(&pool,&s)); }
    assert(!exit_admit(&pool,&s));
    exit_cancel_identity(&pool, 2, release); assert(pool.count == EXIT_COUNT);
    exit_cancel_identity(&pool, 1, release); assert(!pool.count && !pool.bytes);
    for (unsigned i=0; i<10000; i++) {
        s.generation=i+1;
        animation_start(&s.animation, 1, s.generation, 100, 100);
        assert(exit_admit(&pool,&s));
        exit_expire(&pool, 200, release);
        assert(!pool.count && !pool.bytes);
    }
    assert(exit_admit(&pool,&s)); exit_expire(&pool,99,release); assert(!pool.count);
    unsigned long reason[] = {1, 42, 10, 1, ANIMATION_HIDE_WORKSPACE};
    assert(animation_reason_valid(reason,42,0));
    assert(!animation_reason_valid(reason,42,1));
    assert(!animation_reason_valid(reason,43,0));
    reason[3]=1; assert(animation_reason_valid(reason,42,UINT32_MAX));
    reason[3]=UINT32_MAX; assert(!animation_reason_valid(reason,42,1));
    assert(animation_reason_valid(reason,42,0));
    reason[4]=99; assert(!animation_reason_valid(reason,42,0));
    unsigned long close[] = {1,42,100};
    assert(animation_close_valid(close,3,0,42));
    assert(!animation_close_valid(close,2,0,42));
    assert(!animation_close_valid(close,3,1,42));
    close[2]=1001; assert(!animation_close_valid(close,3,0,42));
    puts("PASS: production exit pool byte/count/TTL, completion, cancellation, 10000 cycles and lifecycle validation");
}
