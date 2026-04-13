#include "scheduler.h"

thread_hot_t *current __attribute__((aligned(64))) = NULL;


thread_hot_t *ring_data[RING_CAP] __attribute__((aligned(64)));


ring_idx_t ring_idx __attribute__((aligned(64)));

void sched_init(void)
{
    ring_init();
}


