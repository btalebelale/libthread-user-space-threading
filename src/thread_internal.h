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

// HOT : touché à chaque yield/switch — 16 bytes, 4 par cache line
// PAS d'aligned(64) par struct : on aligne le TABLEAU pour que
// 4 thread_hot tiennent dans 1 cache line (64 / 16 = 4)
typedef struct thread_hot
{
    void *rsp;           // 8B — stack pointer (sauvegardé par context_switch)
    uint32_t state;      // 4B — état courant
    uint32_t _pad;       // 4B — padding alignement
} thread_hot_t;

// COLD : touché uniquement à create/join/exit
typedef struct thread_cold
{
    void *retval;                // 8B — valeur de retour du thread
    void *stack_base;            // 8B — base du stack (NULL pour le main)
    struct thread_hot *waiting;  // 8B — thread en attente de join sur celui-ci
    int valgrind_stackid;        // 4B — ID Valgrind pour register/deregister
} thread_cold_t;

// Global pointer to the currently running thread
extern thread_hot_t *current;

// Défini dans context_switch.S
extern void context_switch(void **old_rsp, void *new_rsp);
extern void context_restore(void *new_rsp) __attribute__((__noreturn__));
extern void thread_trampoline(void);

#endif
