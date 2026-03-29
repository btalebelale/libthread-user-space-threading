// Implémentation de pool.h avec un buffer circulaire pré-alloué.
//
// Propriétés :
//   - put/remove aux deux extrémités : O(1)
//   - get_at(i)                      : O(1)
//   - put_at(i) / remove_at(i)       : O(n/2) — on décale le côté le plus court
//   - Zéro allocation dynamique après pool_init
//   - Localité cache optimale (tableau contigu)
//
// Layout mémoire :
//   data[(head + i) % cap]  donne le i-ème élément logique

#include <stdlib.h>

#include "pool.h"
#include "log_sys.h"

struct pool {
    thread_m **data;
    int head; // indice du premier élément logique (bouge à chaque remove_first)
    int size;
    int cap;
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
    p->data = malloc(sizeof(thread_m *) * cap);
    if (p->data == NULL) {
        LOG_E("Echec de l'allocation du buffer");
        free(p);
        return NULL;
    }
    p->head = 0;
    p->size = 0;
    p->cap  = cap;
    return p;
}

void pool_free(pool *p) {
    if (p == NULL) {
        LOG_E("p est NULL");
        return;
    }
    free(p->data);
    free(p);
}

int is_pool_empty(pool *p) {
    if (p == NULL) {
        LOG_E("p est NULL");
        return -1;
    }
    return p->size == 0;
}

int pool_size(pool *p) {
    if (p == NULL) {
        LOG_E("p est NULL");
        return -1;
    }
    return p->size;
}

// Indice physique du i-ème élément logique.
static inline int phys(pool *p, int i) {
    return (p->head + i) % p->cap;
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
    return p->data[phys(p, i)];
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
    p->data[phys(p, p->size)] = th;
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
    if (p->size >= p->cap) {
        LOG_E("Pool pleine (cap=%d)", p->cap);
        return -1;
    }
    p->head = (p->head - 1 + p->cap) % p->cap;
    p->data[p->head] = th;
    p->size++;
    return 0;
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
    if (idx <= p->size / 2) {
        // Décale le côté gauche d'un cran vers la tête (moins de mouvements)
        p->head = (p->head - 1 + p->cap) % p->cap;
        for (int j = 0; j < idx; j++)
            p->data[phys(p, j)] = p->data[phys(p, j + 1)];
    } else {
        // Décale le côté droit d'un cran vers la queue
        for (int j = p->size; j > idx; j--)
            p->data[phys(p, j)] = p->data[phys(p, j - 1)];
    }
    p->data[phys(p, idx)] = th;
    p->size++;
    return 0;
}

thread_m *pool_remove_first(pool *p) {
    if (p == NULL) {
        LOG_E("p est NULL");
        return NULL;
    }
    if (p->size == 0) {
        LOG_E("Pool vide");
        return NULL;
    }
    thread_m *th = p->data[p->head];
    p->head = (p->head + 1) % p->cap;
    p->size--;
    return th;
}

thread_m *pool_remove_last(pool *p) {
    if (p == NULL) {
        LOG_E("p est NULL");
        return NULL;
    }
    if (p->size == 0) {
        LOG_E("Pool vide");
        return NULL;
    }
    p->size--;
    return p->data[phys(p, p->size)];
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
    thread_m *th = p->data[phys(p, idx)];
    if (idx <= p->size / 2) {
        // Décale le côté gauche d'un cran vers la queue
        for (int j = idx; j > 0; j--)
            p->data[phys(p, j)] = p->data[phys(p, j - 1)];
        p->head = (p->head + 1) % p->cap;
    } else {
        // Décale le côté droit d'un cran vers la tête
        for (int j = idx; j < p->size - 1; j++)
            p->data[phys(p, j)] = p->data[phys(p, j + 1)];
    }
    p->size--;
    return th;
}
