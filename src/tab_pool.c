#pragma GCC optimize("Ofast,unroll-loops")

// Implémentation de pool.h avec un tableau simple.
// remove_first est O(n) (shift).

#include <stdlib.h>
#include "pool.h"

struct pool {
    thread_m **data;
    int cap;
    int size;
} __attribute__((aligned(64)));

pool *pool_init(int cap) {
    pool *p = aligned_alloc(64, sizeof(*p));
    if (__builtin_expect(!p, 0))
        return NULL;
    p->data = malloc(sizeof(thread_m *) * cap);
    if (__builtin_expect(!p->data, 0)) {
        free(p);
        return NULL;
    }
    p->cap  = cap;
    p->size = 0;
    return p;
}

void pool_free(pool *p) {
    free(p->data);
    free(p);
}

int is_pool_empty(pool *p) {
    if (__builtin_expect(!p, 0))
        return 1;
    return p->size == 0;
}

int pool_put_last(pool *p, thread_m *th) {
    p->data[p->size++] = th;
    return 0;
}

thread_m *pool_remove_first(pool *p) {
    thread_m *th = p->data[0];
    p->size--;
    for (int i = 0; i < p->size; i++)
        p->data[i] = p->data[i + 1];
    return th;
}
