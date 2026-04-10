#pragma GCC optimize("Ofast,unroll-loops")

// Implémentation de pool.h avec STAILQ et nœuds pré-alloués.


#include <stdlib.h>
#include <sys/queue.h>
#include "pool.h"

struct pool_node {
    thread_m *thread;
    STAILQ_ENTRY(pool_node) link;
};

STAILQ_HEAD(pool_head, pool_node);

struct pool {
    struct pool_head active;
    struct pool_head free_list;
    struct pool_node *nodes;
} __attribute__((aligned(64)));

pool *pool_init(int cap) {
    pool *p = aligned_alloc(64, sizeof(*p));
    if (__builtin_expect(!p, 0))
        return NULL;
    p->nodes = malloc(sizeof(struct pool_node) * cap);
    if (__builtin_expect(!p->nodes, 0)) {
        free(p);
        return NULL;
    }
    STAILQ_INIT(&p->active);
    STAILQ_INIT(&p->free_list);
    for (int i = 0; i < cap; i++)
        STAILQ_INSERT_TAIL(&p->free_list, &p->nodes[i], link);
    return p;
}

void pool_free(pool *p) {
    free(p->nodes);
    free(p);
}

int is_pool_empty(pool *p) {
    if (__builtin_expect(!p, 0))
        return 1;
    return STAILQ_EMPTY(&p->active);
}

int pool_put_last(pool *p, thread_m *th) {
    struct pool_node *node = STAILQ_FIRST(&p->free_list);
    STAILQ_REMOVE_HEAD(&p->free_list, link);
    node->thread = th;
    STAILQ_INSERT_TAIL(&p->active, node, link);
    return 0;
}

thread_m *pool_remove_first(pool *p) {
    struct pool_node *node = STAILQ_FIRST(&p->active);
    STAILQ_REMOVE_HEAD(&p->active, link);
    thread_m *th = node->thread;
    STAILQ_INSERT_HEAD(&p->free_list, node, link);
    return th;
}
