#ifndef __SCHEDULER_H__
#define __SCHEDULER_H__

#include "thread_internal.h"
#include "thread_common.h"

#define SCHED_PREV(t)     (THREAD_COLD(t)->sched_prev)
#define SCHED_PRIORITY(t) (THREAD_COLD(t)->priority)

static inline __attribute__((always_inline)) void sched_enqueue(thread_hot_t *t)
{

    thread_hot_t *prev = SCHED_PREV(current);
    t->sched_next = current;
    SCHED_PREV(t) = prev;
    prev->sched_next = t;
    SCHED_PREV(current) = t;
}

static inline __attribute__((always_inline)) thread_hot_t *sched_dequeue_fifo(void)
{

    thread_hot_t *t = current->sched_next;
    current->sched_next = t->sched_next;
    SCHED_PREV(t->sched_next) = current;
    return t;
}

static inline __attribute__((always_inline)) int is_sched_empty(void)
{

    return current->sched_next == current;
}

static inline __attribute__((always_inline)) void sched_remove(thread_hot_t *t)
{

    SCHED_PREV(t)->sched_next = t->sched_next;
    SCHED_PREV(t->sched_next) = SCHED_PREV(t);
}

static inline __attribute__((always_inline)) void sched_replace(thread_hot_t *old,
                                                                 thread_hot_t *new_t)
{

    new_t->sched_next = old->sched_next;
    SCHED_PREV(new_t) = SCHED_PREV(old);
    if (old->sched_next == old)
    {

        new_t->sched_next = new_t;
        SCHED_PREV(new_t) = new_t;
    }
    else
    {
        SCHED_PREV(new_t->sched_next) = new_t;
        SCHED_PREV(new_t)->sched_next = new_t;
    }
}

#ifdef USE_PRIORITY

static inline __attribute__((always_inline)) thread_hot_t *sched_pick_highest(void){
    thread_hot_t *best = current->sched_next;
    for (thread_hot_t *t = best->sched_next; t != current; t = t->sched_next)
        if (SCHED_PRIORITY(t) > SCHED_PRIORITY(best))
            best = t;
    return best;
}

#endif

static inline __attribute__((always_inline)) thread_hot_t *sched_pick_next(void)
{
#ifdef USE_PRIORITY
    return sched_pick_highest();
#else
    return SCHED_PREV(current);
#endif
}

void sched_init(void);

#endif
