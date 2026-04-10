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

// HOT : touché à chaque yield — 16 bytes, 4 par cache line
typedef struct thread_hot
{
    void *rsp;          
    uint32_t state;      
    uint32_t _pad;
} thread_hot_t;

// COLD : touché uniquement à create/join/exit
typedef struct thread_cold
{
    void *retval;                
    void *stack_base;            
    struct thread_hot *waiting;  
    int valgrind_stackid;        
} thread_cold_t;

// Global pointer to the currently running thread
extern thread_hot_t *current;

// Défini dans context_switch.S
extern void context_switch(void **old_rsp, void *new_rsp);
extern void context_restore(void *new_rsp) __attribute__((__noreturn__));
extern void thread_trampoline(void);

#endif
