// Implémentation de pool.h avec STAILQ et nœuds pré-alloués.
// Les nœuds sont alloués en bloc à pool_init et gérés via une free-list.
// Aucun malloc/free après l'initialisation : performances maximales.

#include <stdlib.h>
#include <sys/queue.h>

#include "pool.h"
#include "log_sys.h"

struct pool_node {
    thread_m *thread;
    STAILQ_ENTRY(pool_node) link; // utilisé dans active OU free, jamais les deux
};

STAILQ_HEAD(pool_head, pool_node);

struct pool {
    struct pool_head active;    // nœuds en service
    struct pool_head free;      // nœuds disponibles
    struct pool_node *nodes;    // tableau pré-alloué (unique allocation)
    int cap;
    int size;
};

pool *pool_init(int cap) {
    if (cap <= 0) {
        LOG_E("cap invalide (%d)", cap);
        return NULL;
    }
    pool *p = malloc(sizeof(*p));
    if (p == NULL) {
        LOG_E("Echec de l'allocation d'une pool");
        return NULL;
    }
    p->nodes = malloc(sizeof(struct pool_node) * cap);
    if (p->nodes == NULL) {
        LOG_E("Echec de l'allocation des nœuds");
        free(p);
        return NULL;
    }
    STAILQ_INIT(&p->active);
    STAILQ_INIT(&p->free);
    for (int i = 0; i < cap; i++)
        STAILQ_INSERT_TAIL(&p->free, &p->nodes[i], link);
    p->cap  = cap;
    p->size = 0;
    return p;
}

void pool_free(pool *p) {
    if (p == NULL) {
        LOG_E("p est NULL");
        return;
    }
    free(p->nodes);
    free(p);
}

int is_pool_empty(pool *p) {
    if (p == NULL) {
        LOG_E("p est NULL");
        return -1;
    }
    return STAILQ_EMPTY(&p->active);
}

int pool_size(pool *p) {
    if (p == NULL) {
        LOG_E("p est NULL");
        return -1;
    }
    return p->size;
}

// Emprunte un nœud de la free-list. NULL si pool pleine.
static struct pool_node *node_alloc(pool *p) {
    if (STAILQ_EMPTY(&p->free)) {
        LOG_E("Pool pleine (cap=%d)", p->cap);
        return NULL;
    }
    struct pool_node *node = STAILQ_FIRST(&p->free);
    STAILQ_REMOVE_HEAD(&p->free, link);
    return node;
}

// Retourne un nœud à la free-list.
static void node_free(pool *p, struct pool_node *node) {
    STAILQ_INSERT_HEAD(&p->free, node, link);
}

// Retourne le nœud actif à la position i, NULL si hors bornes.
static struct pool_node *node_at(pool *p, int i) {
    struct pool_node *node = STAILQ_FIRST(&p->active);
    for (int j = 0; j < i && node != NULL; j++)
        node = STAILQ_NEXT(node, link);
    return node;
}

thread_m *pool_get_at(pool *p, int i) {
    if (p == NULL) {
        LOG_E("p est NULL");
        return NULL;
    }
    if (i < 0 || i >= p->size) {
        LOG_E("Index %d invalide (size=%d)", i, p->size);
        return NULL;
    }
    return node_at(p, i)->thread;
}

thread_m *pool_get_first(pool *p) {
    if (p == NULL) {
        LOG_E("p est NULL");
        return NULL;
    }
    return pool_get_at(p, 0);
}

thread_m *pool_get_last(pool *p) {
    if (p == NULL) {
        LOG_E("p est NULL");
        return NULL;
    }
    return pool_get_at(p, p->size - 1);
}

int pool_put_at(pool *p, thread_m *th, int idx) {
    if (p == NULL) {
        LOG_E("p est NULL");
        return -1;
    }
    if (th == NULL) {
        LOG_E("th est NULL");
        return -1;
    }
    if (idx < 0 || idx > p->size) {
        LOG_E("Index %d invalide (size=%d)", idx, p->size);
        return -1;
    }
    struct pool_node *node = node_alloc(p);
    if (node == NULL)
        return -1;
    node->thread = th;
    if (idx == 0) {
        STAILQ_INSERT_HEAD(&p->active, node, link);
    } else {
        struct pool_node *prev = node_at(p, idx - 1);
        STAILQ_INSERT_AFTER(&p->active, prev, node, link);
    }
    p->size++;
    return 0;
}

int pool_put_first(pool *p, thread_m *th) {
    if (p == NULL) {
        LOG_E("p est NULL");
        return -1;
    }
    if (th == NULL) {
        LOG_E("th est NULL");
        return -1;
    }
    return pool_put_at(p, th, 0);
}

int pool_put_last(pool *p, thread_m *th) {
    if (p == NULL) {
        LOG_E("p est NULL");
        return -1;
    }
    if (th == NULL) {
        LOG_E("th est NULL");
        return -1;
    }
    struct pool_node *node = node_alloc(p);
    if (node == NULL)
        return -1;
    node->thread = th;
    STAILQ_INSERT_TAIL(&p->active, node, link);
    p->size++;
    return 0;
}

thread_m *pool_remove_at(pool *p, int idx) {
    if (p == NULL) {
        LOG_E("p est NULL");
        return NULL;
    }
    if (idx < 0 || idx >= p->size) {
        LOG_E("Index %d invalide (size=%d)", idx, p->size);
        return NULL;
    }
    struct pool_node *node;
    if (idx == 0) {
        node = STAILQ_FIRST(&p->active);
        STAILQ_REMOVE_HEAD(&p->active, link);
    } else {
        struct pool_node *prev = node_at(p, idx - 1);
        node = STAILQ_NEXT(prev, link);
        STAILQ_REMOVE(&p->active, node, pool_node, link);
    }
    thread_m *th = node->thread;
    node_free(p, node);
    p->size--;
    return th;
}

thread_m *pool_remove_first(pool *p) {
    if (p == NULL) {
        LOG_E("p est NULL");
        return NULL;
    }
    return pool_remove_at(p, 0);
}

thread_m *pool_remove_last(pool *p) {
    if (p == NULL) {
        LOG_E("p est NULL");
        return NULL;
    }
    return pool_remove_at(p, p->size - 1);
}
