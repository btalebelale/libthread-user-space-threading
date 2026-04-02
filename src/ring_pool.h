#ifndef __RING_POOL_H__
#define __RING_POOL_H__

// Ring buffer FIFO inliné pour le scheduler hot path.
// Invariant : cap est toujours une puissance de 2.
//   → (x & mask) au lieu de (x % cap) : AND ~1 cycle vs IDIV ~35 cycles

#include "thread_internal.h"
#include <stdlib.h>
#include <string.h>

struct pool {
    thread_hot_t **data;
    unsigned head;
    unsigned tail;
    unsigned mask;
} __attribute__((aligned(64)));

typedef struct pool pool;

static inline unsigned next_pow2(unsigned v) {
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    return v + 1;
}

__attribute__((cold))
static inline pool *pool_init(int cap) {
    if (__builtin_expect(cap <= 0, 0))
        return NULL;
    pool *p = aligned_alloc(64, sizeof(*p));
    if (__builtin_expect(p == NULL, 0))
        return NULL;
    unsigned real_cap = next_pow2((unsigned)cap);
    p->data = malloc(sizeof(thread_hot_t *) * real_cap);
    if (__builtin_expect(p->data == NULL, 0)) {
        free(p);
        return NULL;
    }
    p->head = 0;
    p->tail = 0;
    p->mask = real_cap - 1;
    return p;
}

__attribute__((cold))
static inline void pool_free(pool *p) {
    free(p->data);
    free(p);
}

static inline __attribute__((always_inline))
int is_pool_empty(pool *p) {
    if (__builtin_expect(p == NULL, 0))
        return 1;
    return p->head == p->tail;
}

static inline __attribute__((always_inline))
int pool_put_last(pool *p, thread_hot_t *th) {
    p->data[p->tail & p->mask] = th;
    p->tail++;
    return 0;
}

static inline __attribute__((always_inline))
thread_hot_t *pool_remove_first(pool *p) {
    thread_hot_t *th = p->data[p->head & p->mask];
    p->head++;
    __builtin_prefetch(&p->data[p->head & p->mask], 0, 3);
    return th;
}

#endif /* __RING_POOL_H__ */
