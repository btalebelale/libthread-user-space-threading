#ifndef __SCHEDULER_MN_H__
#define __SCHEDULER_MN_H__

#include "thread_internal.h"

void runq_init(int nworkers);
void runq_shutdown_broadcast(void);

void runq_push(thread_hot_t *t);

thread_hot_t *runq_pop_block(int self_id);

void lazy_stack_alloc(thread_hot_t *t);

void schedule(void);

void schedule_park(void (*cb)(void *), void *arg);

__attribute__((__noreturn__)) void schedule_exit(void);

__attribute__((__noreturn__)) void worker_loop(void);

#endif
