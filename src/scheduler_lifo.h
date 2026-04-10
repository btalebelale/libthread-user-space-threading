#ifndef __SCHEDULER_LIFO_H__
#define __SCHEDULER_LIFO_H__

#include "thread_internal.h"
#include "ring_pool.h"

extern pool *ready_queue;
extern thread_hot_t *current;



static inline void sched_enqueue(thread_hot_t *t) {
    pool_put_last(ready_queue, t);
}

static inline thread_hot_t *sched_dequeue_fifo(void) {
    return pool_remove_last(ready_queue);
}

static inline thread_hot_t *sched_dequeue_lifo(void) {
    return pool_remove_last(ready_queue);
}

static inline int is_sched_empty(void) {
    return is_pool_empty(ready_queue);
}

// Cold path : init et cleanup
void sched_init(void);
void sched_cleanup(void);

#endif
