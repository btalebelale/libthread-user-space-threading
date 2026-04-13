#ifndef __SCHEDULER_H__
#define __SCHEDULER_H__

#include "thread_internal.h"
#include "ring_pool.h"

extern thread_hot_t *current;

static inline void sched_enqueue(thread_hot_t *t) {
    ring_put_last(t);
}

static inline thread_hot_t *sched_dequeue_fifo(void) {
    return ring_remove_first();
}

static inline thread_hot_t *sched_dequeue_lifo(void) {
    return ring_remove_last();
}

static inline int is_sched_empty(void) {
    return ring_empty();
}

// Cold path : init et cleanup
void sched_init(void);


#endif
