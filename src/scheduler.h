#ifndef __SCHEDULER_H__
#define __SCHEDULER_H__

#include <stdlib.h>
#include <sys/queue.h>
#include <ucontext.h>

#include "thread.h"

typedef enum {
    READY,
    RUNNING,
    BLOCKED,
    FINISHED
} state_t;

typedef struct thread {
    ucontext_t ctx; // le contexte du thread 
    void *stack;  // L'adresse de la pile (pour free )

    void *retval; // La valeur de retour du thread 
    state_t state;  // l'état

    STAILQ_ENTRY(thread) link;  // Le next dans la queue
    struct thread *waiting; // Le thread qui m'attend attend
} thread_m;


extern thread_m *current;



extern struct thread_t *current_thread;


// initialise la liste des threads
void sched_init(void);

// mets le thread au tail de la liste des threads 
void sched_enqueue(thread_m *t);

// Remove le premier thread ready de la liste des threads
thread_m *sched_dequeue(void);

// vérifie si FIFO est vide 
int sched_empty(void);

#endif