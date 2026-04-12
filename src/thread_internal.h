#ifndef __THREAD_INTERNAL_H__
#define __THREAD_INTERNAL_H__

#include "thread.h"
#include <stdlib.h>
#include <stdint.h>

// Thread states
typedef enum
{
    READY,
    RUNNING,
    BLOCKED,
    FINISHED
} state_t;

// HOT : touché à chaque yield — 8 bytes seulement (rsp), 8 par cache line
// state déplacé dans cold car jamais accédé dans yield (le hot path)
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
} thread_cold_t;

// Global pointer to the currently running thread
extern thread_hot_t *current;

// Défini dans context_switch.S — hidden pour eviter PLT dans le hot path
__attribute__((visibility("hidden")))
extern void context_switch(void **old_rsp, void *new_rsp);
__attribute__((visibility("hidden"), __noreturn__))
extern void context_restore(void *new_rsp);
__attribute__((visibility("hidden")))
extern void thread_trampoline(void);

#endif
