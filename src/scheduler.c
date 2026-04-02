#include "scheduler.h"

#define SCHED_MAX_THREADS 16384

pool *ready_queue = NULL;
thread_hot_t *current = NULL;

void sched_init(void)
{
    ready_queue = pool_init(SCHED_MAX_THREADS);
}

void sched_cleanup(void)
{
    if (ready_queue)
    {
        pool_free(ready_queue);
        ready_queue = NULL;
    }
}
