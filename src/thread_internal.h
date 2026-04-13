#ifndef __THREAD_INTERNAL_H__
#define __THREAD_INTERNAL_H__

#include "thread.h"
#include <stdint.h>
#include <stdlib.h>

// Thread states
typedef enum
{
    READY,
    RUNNING,
    BLOCKED,
    FINISHED
} state_t;

// HOT : touché à chaque yield
typedef struct thread_hot
{
    void *rsp;
} thread_hot_t;

// COLD : touché uniquement à create/join/exit
typedef struct thread_cold
{
    void *retval;
    void *stack_base;
    struct thread_hot *waiting;
    uint32_t state;
    int valgrind_stackid;
    void *(*func)(void *); // pour déduplication
    void *func_arg;        // pour déduplication
} thread_cold_t;

extern thread_hot_t *current;

__attribute__((visibility("hidden"))) extern void context_switch(void **old_rsp, void *new_rsp);
__attribute__((visibility("hidden"), __noreturn__)) extern void context_restore(void *new_rsp);
__attribute__((visibility("hidden"))) extern void thread_trampoline(void);

#endif
