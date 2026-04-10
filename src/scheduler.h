#ifndef __SCHEDULER_H__
#define __SCHEDULER_H__



#if defined(SCHED_FIFO)
#include "scheduler_fifo.h"
#elif defined(SCHED_LIFO)
#include "scheduler_lifo.h"
#else
#include "scheduler_hybrid.h"
#endif

#endif
