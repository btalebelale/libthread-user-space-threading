#ifndef __POOL_H__
#define __POOL_H__

#include "thread_internal.h"

typedef struct pool pool;

/*Remarque lorsqu'ici on numérot les thread,
  se n'est pas (forcément) l'ordre correspondant à celui d'exécution.
  Ce dernier sera choisie par l'ordonnanceur. */

// Initialise une pool de thread de taille cap.
// Vide au début. Renvoie NULL en cas d'échec.
pool *pool_init(int cap);

// Libére l'espace mémoire allouer à la pool.
// Ne libère pas les threads.
void pool_free(pool *p);

// Vérifie si la pool est vide. Renvoie O si ce n'est pas le cas. Une autre
// valeur sinon. -1 si erreur.
int is_pool_empty(pool *p);

// Revoie la taille de la pool. -1 si erreur.
int pool_size(pool *p);

// Renvoie le ième thread. Renvoie NULL en cas d'échec.
thread_m *pool_get_at(pool *p, int i);

// Renvoie le premier thread. Renvoie NULL en cas d'échec.
thread_m *pool_get_first(pool *p);

// Renvoie le dernier thread. Renvoie NULL en cas d'échec.
thread_m *pool_get_last(pool *p);

// Met le thread th à la ième position. Renvoie -1 si echec 0 sinon.
// On suppose que si il y 'avait un élément à la position i il est décalé vers
// la droite.
int pool_put_at(pool *p, thread_m *th, int i);

// Met le thread à la première position.  Renvoie -1 si echec 0 sinon.
// A un comportement similaire à la fonction ci dessus avec idx = 0
int pool_put_first(pool *p, thread_m *th);

// Met le thread à la dernière position.  Renvoie -1 si echec 0 sinon.
int pool_put_last(pool *p, thread_m *th);

// Retire et renvoie le thread à la ième position. Renvoie NULL en cas d'échec.
thread_m *pool_remove_at(pool *p, int i);

// Retire et renvoie le premier thread. Renvoie NULL en cas d'échec.
thread_m *pool_remove_first(pool *p);

// Retire et renvoie le dernier thread. Renvoie NULL en cas d'échec.
thread_m *pool_remove_last(pool *p);

#endif /* __POOL_H__ */
