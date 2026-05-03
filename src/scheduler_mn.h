#ifndef __SCHEDULER_MN_H__
#define __SCHEDULER_MN_H__

#include "thread_internal.h"

/* nworkers doit être connu à l'init : la table de runqueues est dimensionnée
 * dessus pour éviter le faux-partage entre runqueues actives et inutilisées. */
void runq_init(int nworkers);
void runq_destroy(void);
void runq_shutdown_broadcast(void);

/* Push sur la deque du worker courant (self_worker->id).
 * Étape 1 : pas de migration — le placement est purement local. */
void runq_push(thread_hot_t *t);

/* Pop bloquant sur la deque du worker self_id. Retourne NULL si shutdown. */
thread_hot_t *runq_pop_block(int self_id);

/* lazy_stack_alloc lives in thread_mn.c */
void lazy_stack_alloc(thread_hot_t *t);

/* Sauvegarde rsp courant, push current dans la runqueue, saute sur sched_rsp. */
void schedule(void);

/* Comme schedule, mais ne push pas current. cb sera appelé par worker_loop
 * juste après le retour côté scheduler (release-then-park). */
void schedule_park(void (*cb)(void *), void *arg);

/* Quitte le thread courant : saute directement sur sched_rsp, sans sauver. */
__attribute__((__noreturn__)) void schedule_exit(void);

/* Boucle principale de chaque worker. Tourne sur la sched_stack. */
__attribute__((__noreturn__)) void worker_loop(void);

#endif
