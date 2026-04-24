#ifndef __THREAD_INTERNAL_H__
#define __THREAD_INTERNAL_H__

#include "thread.h"
#include <setjmp.h>
#include <stdlib.h>
#include <sys/queue.h>

// Thread states
typedef enum
{
    READY,
    RUNNING,
    BLOCKED,
    FINISHED
} state_t;

typedef struct thread
{
    jmp_buf env;       // sauvegarde les registres du thread
    void *stack;       // pile du thread
    size_t stack_size; // la taille

    void *retval;           // Thread's return value
    state_t state;          // Current state of the thread
    int valgrind_stackid;   // Valgrind ID to register/deregister the custom stack
    struct thread *waiting; // Pointer to the thread waiting for this one to join

    void *(*func)(void *);
    void *func_arg;

    STAILQ_ENTRY(thread) link; // Le next du thread
} thread_m;


typedef struct thread_mutex_internal{
    int locked;  
    STAILQ_HEAD(, thread) waiting;  // une file de threads bloqués sur ce mutex
} thread_mutex_m;

// Global pointer to the currently running thread
extern thread_m *current;

#endif