// Implémentation de pool.h avec STAILQ et wrapper alloué dynamiquement.
// Surcharge : un malloc/free par put/remove.
// Avantage : pas de capacité maximale imposée au nœud (seul cap l'est).

#include <stdlib.h>
#include <sys/queue.h>

#include "pool.h"
#include "../debug/log_sys.h"

struct pool_node {
    thread_m *thread;
    STAILQ_ENTRY(pool_node) link;
};

STAILQ_HEAD(pool_head, pool_node);

struct pool {
    struct pool_head head;
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
    STAILQ_INIT(&p->head);
    p->cap  = cap;
    p->size = 0;
    return p;
}

void pool_free(pool *p) {
    if (p == NULL) {
        LOG_E("p est NULL");
        return;
    }
    struct pool_node *node;
    while (!STAILQ_EMPTY(&p->head)) {
        node = STAILQ_FIRST(&p->head);
        STAILQ_REMOVE_HEAD(&p->head, link);
        free(node);
    }
    free(p);
}

int pool_is_empty(pool *p) {
    if (p == NULL) {
        LOG_E("p est NULL");
        return -1;
    }
    return STAILQ_EMPTY(&p->head);
}

int pool_size(pool *p) {
    if (p == NULL) {
        LOG_E("p est NULL");
        return -1;
    }
    return p->size;
}

// Retourne le nœud à la position i, NULL si hors bornes.
static struct pool_node *node_at(pool *p, int i) {
    struct pool_node *node = STAILQ_FIRST(&p->head);
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
    if (p->size >= p->cap) {
        LOG_E("Pool pleine (cap=%d)", p->cap);
        return -1;
    }
    struct pool_node *node = malloc(sizeof(*node));
    if (node == NULL) {
        LOG_E("Echec allocation pool_node");
        return -1;
    }
    node->thread = th;
    if (idx == 0) {
        STAILQ_INSERT_HEAD(&p->head, node, link);
    } else {
        struct pool_node *prev = node_at(p, idx - 1);
        STAILQ_INSERT_AFTER(&p->head, prev, node, link);
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
    if (p->size >= p->cap) {
        LOG_E("Pool pleine (cap=%d)", p->cap);
        return -1;
    }
    struct pool_node *node = malloc(sizeof(*node));
    if (node == NULL) {
        LOG_E("Echec allocation pool_node");
        return -1;
    }
    node->thread = th;
    STAILQ_INSERT_TAIL(&p->head, node, link);
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
        node = STAILQ_FIRST(&p->head);
        STAILQ_REMOVE_HEAD(&p->head, link);
    } else {
        struct pool_node *prev = node_at(p, idx - 1);
        node = STAILQ_NEXT(prev, link);
        STAILQ_REMOVE(&p->head, node, pool_node, link);
    }
    thread_m *th = node->thread;
    free(node);
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
