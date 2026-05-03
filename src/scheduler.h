#ifndef __SCHEDULER_H__
#define __SCHEDULER_H__

#include "thread_internal.h"

// current est TOUJOURS dans la liste circulaire.
// Les threads READY sont dans la liste avec current.
// yield = avancer current au suivant (pas d'enqueue/dequeue).

static inline __attribute__((always_inline)) void sched_enqueue(thread_hot_t *t)
{
    // Inserer avant current (FIFO : t sera atteint en dernier dans le cycle)
    thread_hot_t *prev = current->sched_prev;
    t->sched_next = current;
    t->sched_prev = prev;
    prev->sched_next = t;
    current->sched_prev = t;
}

static inline __attribute__((always_inline)) thread_hot_t *sched_dequeue_fifo(void)
{
    // Retirer le thread juste apres current (le plus ancien)
    thread_hot_t *t = current->sched_next;
    current->sched_next = t->sched_next;
    t->sched_next->sched_prev = current;
    return t;
}

static inline __attribute__((always_inline)) int is_sched_empty(void)
{
    // current est seul dans la liste
    return current->sched_next == current;
}

static inline __attribute__((always_inline)) void sched_remove(thread_hot_t *t)
{
    // Retirer t de la liste (pour block/exit)
    t->sched_prev->sched_next = t->sched_next;
    t->sched_next->sched_prev = t->sched_prev;
}

static inline __attribute__((always_inline)) void sched_replace(thread_hot_t *old,
                                                                 thread_hot_t *new_t)
{
    // Remplacer old par new_t dans la liste (pour exit avec waiter)
    new_t->sched_next = old->sched_next;
    new_t->sched_prev = old->sched_prev;
    if (old->sched_next == old)
    {
        // old etait seul → new_t seul
        new_t->sched_next = new_t;
        new_t->sched_prev = new_t;
    }
    else
    {
        new_t->sched_next->sched_prev = new_t;
        new_t->sched_prev->sched_next = new_t;
    }
}

#ifdef USE_PRIORITY

static inline __attribute__((always_inline)) thread_hot_t *sched_pick_highest(void){
    thread_hot_t *best = current->sched_next;
    for (thread_hot_t *t = best->sched_next; t != current; t = t->sched_next)
        if (t->priority > best->priority)
            best = t;
    return best;
}

#endif

void sched_init(void);


#endif
