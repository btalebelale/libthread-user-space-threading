#include "scheduler.h"

thread_hot_t *current __attribute__((aligned(64))) = NULL;

// Ring buffer statique — definitions uniques (declarees extern dans ring_pool.h)
thread_hot_t *ring_data[RING_CAP] __attribute__((aligned(64)));

// head et tail sur la même cache line : mono-thread coopératif,
// toujours accédés ensemble (enqueue+dequeue dans yield), pas de false sharing
ring_idx_t ring_idx __attribute__((aligned(64)));

void sched_init(void)
{
    ring_init();
}

void sched_cleanup(void)
{
    // Ring buffer statique — rien a liberer
}
