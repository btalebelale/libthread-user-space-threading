#ifndef __SCHEDULER_H__
#define __SCHEDULER_H__

#include "thread_internal.h"

// Initializes the thread ready queue
void sched_init(void);

// Adds a thread to the tail of the ready queue 
void sched_enqueue(thread_m *t);

// Removes and returns the first thread from the ready queue
thread_m *sched_dequeue(void);

// Checks if the ready queue is empty (returns 1 if empty, 0 otherwise)
int sched_empty(void);

#endif