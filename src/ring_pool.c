#pragma GCC optimize("Ofast,unroll-loops")

// Buffer circulaire FIFO pour le scheduler.
//
// Invariant : cap est toujours une puissance de 2.
//   → (x % cap) se compile en (x & mask), soit AND au lieu de IDIV (~1 vs ~35 cycles)
//
// head et tail sont des compteurs monotones (jamais wrappés).
// L'indice physique est  idx & mask.
// size = tail - head.

#include <stdlib.h>
#include <string.h>

#include "pool.h"
#include "log_sys.h"

struct pool {
    thread_m **data;
    unsigned head;
    unsigned tail;
    unsigned mask;
} __attribute__((aligned(64)));

// Arrondit cap à la puissance de 2 supérieure ou égale.
static inline unsigned next_pow2(unsigned v) {
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    return v + 1;
}

pool *pool_init(int cap) {
    if (__builtin_expect(cap <= 0, 0))
        return NULL;
    pool *p = aligned_alloc(64, sizeof(*p));
    if (__builtin_expect(p == NULL, 0))
        return NULL;
    unsigned real_cap = next_pow2((unsigned)cap);
    p->data = malloc(sizeof(thread_m *) * real_cap);
    if (__builtin_expect(p->data == NULL, 0)) {
        free(p);
        return NULL;
    }
    p->head = 0;
    p->tail = 0;
    p->mask = real_cap - 1;
    return p;
}

void pool_free(pool *p) {
    free(p->data);
    free(p);
}

int is_pool_empty(pool *p) {
    if (__builtin_expect(p == NULL, 0))
        return 1;
    return p->head == p->tail;
}

// Hot path : pas de NULL check, le scheduler garantit la validité des arguments.
int pool_put_last(pool *p, thread_m *th) {
    p->data[p->tail & p->mask] = th;
    p->tail++;
    return 0;
}

// Hot path : pas de NULL/empty check, le scheduler appelle is_sched_empty avant.
thread_m *pool_remove_first(pool *p) {
    thread_m *th = p->data[p->head & p->mask];
    p->head++;
    // Prefetch le prochain élément en L1 pour le prochain dequeue
    __builtin_prefetch(&p->data[p->head & p->mask], 0, 3);
    return th;
}
