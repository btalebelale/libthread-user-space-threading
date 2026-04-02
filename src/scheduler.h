#ifndef __SCHEDULER_H__
#define __SCHEDULER_H__

#include "thread_internal.h"
#include "ring_pool.h"

extern pool *ready_queue;
extern thread_hot_t *current;

// Hot path : static inline pour garantir l'inlining dans thread_yield/thread_exit
static inline void sched_enqueue(thread_hot_t *t) {
    pool_put_last(ready_queue, t);
}

static inline thread_hot_t *sched_dequeue(void) {
    return pool_remove_first(ready_queue);
}

static inline int is_sched_empty(void) {
    return is_pool_empty(ready_queue);
}

// Cold path : init et cleanup
void sched_init(void);
void sched_cleanup(void);

#endif
