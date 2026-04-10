#pragma GCC optimize("Ofast,unroll-loops")

// Implémentation de pool.h avec STAILQ (liste chaînée).
// malloc/free à chaque put/remove.

#include <stdlib.h>
#include <sys/queue.h>
#include "pool.h"

typedef struct pool_node {
    thread_m *thread;
    STAILQ_ENTRY(pool_node) link;
} pool_node;

STAILQ_HEAD(pool_head, pool_node);

struct pool {
    struct pool_head head;
} __attribute__((aligned(64)));

pool *pool_init(int cap) {
    (void)cap;
    pool *p = aligned_alloc(64, sizeof(*p));
    if (__builtin_expect(!p, 0))
        return NULL;
    STAILQ_INIT(&p->head);
    return p;
}

void pool_free(pool *p) {
    while (!STAILQ_EMPTY(&p->head)) {
        pool_node *node = STAILQ_FIRST(&p->head);
        STAILQ_REMOVE_HEAD(&p->head, link);
        free(node);
    }
    free(p);
}

int is_pool_empty(pool *p) {
    if (__builtin_expect(!p, 0))
        return 1;
    return STAILQ_EMPTY(&p->head);
}

int pool_put_last(pool *p, thread_m *t) {
    pool_node *node = malloc(sizeof(*node));
    if (__builtin_expect(!node, 0))
        return -1;
    node->thread = t;
    STAILQ_INSERT_TAIL(&p->head, node, link);
    return 0;
}

thread_m *pool_remove_first(pool *p) {
    pool_node *node = STAILQ_FIRST(&p->head);
    STAILQ_REMOVE_HEAD(&p->head, link);
    thread_m *t = node->thread;
    free(node);
    return t;
}
