#ifndef __THREAD_INTERNAL_H__
#define __THREAD_INTERNAL_H__

#include "thread.h"
#include <stdlib.h>
#include <ucontext.h>
#include <setjmp.h>

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
    //ucontext_t ctx;       // Thread context
    jmp_buf env;            //  sauvegarde les registres du thread
    void *stack;            // pile du thread
    size_t stack_size;      // la taille 
    void *retval;           // Thread's return value
    state_t state;          // Current state of the thread
    int valgrind_stackid;   // Valgrind ID to register/deregister the custom stack
    struct thread *waiting; // Pointer to the thread waiting for this one to join
    void *(*func)(void *);                                                                                                
    void *func_arg;   
} thread_m;

// Global pointer to the currently running thread
extern thread_m *current;

#endif