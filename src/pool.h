#ifndef __POOL_H__
#define __POOL_H__

#include "thread_internal.h"

typedef struct pool pool;

// Initialise une pool de thread de taille cap.
// Vide au début. Renvoie NULL en cas d'échec.
pool *pool_init(int cap);

// Libère l'espace mémoire alloué à la pool.
// Ne libère pas les threads.
void pool_free(pool *p);

// Vérifie si la pool est vide. Renvoie 0 si ce n'est pas le cas. Une autre valeur sinon. -1 si erreur.
int is_pool_empty(pool *p);

// Met le thread à la dernière position. Renvoie -1 si echec 0 sinon.
int pool_put_last(pool *p, thread_m *th);

// Retire et renvoie le premier thread. Renvoie NULL en cas d'échec.
thread_m *pool_remove_first(pool *p);

#endif /* __POOL_H__ */
