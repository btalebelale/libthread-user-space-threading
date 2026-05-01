#ifndef __THREAD_INTERNAL_H__
#define __THREAD_INTERNAL_H__

#include "thread.h"
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>

// Thread states
typedef enum
{
    READY,
    FINISHED
} state_t;

// HOT : touché à chaque yield
typedef struct thread_hot
{
    void *rsp;
    struct thread_hot *sched_next;
    struct thread_hot *sched_prev;
} thread_hot_t;

// COLD : touché uniquement à create/join/exit
typedef struct thread_cold
{
    void *retval;
    void *stack_base;
    struct thread_hot *waiting;

    struct thread_cold *daddy;
    uint8_t rank;

    uint32_t state;
    int valgrind_stackid;
    void *(*func)(void *); // pour déduplication
    void *func_arg;        // pour déduplication
    jmp_buf *inline_jmpbuf;
    uint16_t refcount;
    uint8_t started;
    thread_sigset_t pending_sigs;  // bitmask: signaux reçus, pas encore consommés
    thread_sigset_t wait_mask;     // bitmask: signaux attendus par thread_sigwait
    uint8_t sig_waiting;           // 1 si le thread est bloqué dans thread_sigwait
    int     received_sig;          // numéro du signal qui l'a réveillé
} thread_cold_t;

extern thread_hot_t *current;

__attribute__((visibility("hidden"))) extern void context_switch(void **old_rsp, void *new_rsp);

__attribute__((visibility("hidden"), __noreturn__)) extern void context_restore(void *new_rsp);

__attribute__((visibility("hidden"))) extern void thread_trampoline(void);


typedef struct mutex_internal                                                                                                         
{                            
    int locked;                                                                                                                       
    thread_hot_t *wait_head;  // premier thread en attente
    thread_hot_t *wait_tail;  // dernier thread en attente                                                                                                       
} mutex_internal_t; 


typedef struct sem_internal {
    int              count;      // valeur courante du sémaphore
    thread_hot_t    *wait_head;  // FIFO des threads bloqués             
    thread_hot_t    *wait_tail;
} sem_internal_t;

#endif
