#include "scheduler.h"
#include "pool.h"

#include <stdlib.h>

#define SCHED_MAX_THREADS 1024

static pool *ready_queue = NULL;

thread_m *current = NULL;

void sched_init() {
    ready_queue = pool_init(SCHED_MAX_THREADS);
}

void sched_enqueue(thread_m *t) {
    pool_put_last(ready_queue, t);
}

thread_m *sched_dequeue(void) {
    return pool_remove_first(ready_queue);
}

int sched_empty(void) {
    return pool_is_empty(ready_queue);
}

void sched_cleanup(void) {
    if (ready_queue) {
        pool_free(ready_queue);
        ready_queue = NULL;
    }
}
