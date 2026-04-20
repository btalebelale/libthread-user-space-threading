#include "scheduler.h"

thread_hot_t *current __attribute__((aligned(64))) = NULL;

void sched_init(void)
{
    // current sera initialisé dans init_system
}
