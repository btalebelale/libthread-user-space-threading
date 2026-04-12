#include "scheduler.h"

thread_hot_t *current = NULL;

// Ring buffer statique — definitions uniques (declarees extern dans ring_pool.h)
thread_hot_t *ring_data[RING_CAP] __attribute__((aligned(64)));
unsigned ring_head __attribute__((aligned(64)));
unsigned ring_tail __attribute__((aligned(64)));

void sched_init(void)
{
    ring_init();
}

void sched_cleanup(void)
{
    // Ring buffer statique — rien a liberer
}
