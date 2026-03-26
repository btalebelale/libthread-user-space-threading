#include "thread.h"
#include "scheduler.h"

#include <ucontext.h>
#include <stdlib.h>
#include <stdio.h> 
#include <valgrind/valgrind.h>

#define STACK_SIZE 65536


static void init_system(void) {
    sched_init();
    
    current = malloc(sizeof(thread_m));
    if (!current) {
        perror("malloc main thread");
        exit(1);
    }    
    getcontext(&current->ctx); 
    current->state = RUNNING;
    current->stack = NULL;     
    current->retval = NULL;
    current->waiting = NULL;
}

thread_t thread_self(void) {
    if (current == NULL) {
        init_system();
    }
    return (thread_t) current;
}

void thread_start(void *(*func)(void *), void *arg) {
    void *ret = func(arg);
    thread_exit(ret);
}

int thread_create(thread_t *newthread, void *(*func)(void *), void *arg) {
    if (current == NULL) {
        init_system();
    }
    thread_m *t = malloc(sizeof(thread_m));
    if (!t) return -1;

    if (getcontext(&t->ctx) == -1) {
        free(t);
        return -1;
    }

    t->stack = malloc(STACK_SIZE);
    if (!t->stack) {
        free(t);
        return -1;
    }

    t->ctx.uc_stack.ss_sp = t->stack;
    t->ctx.uc_stack.ss_size = STACK_SIZE;
    t->ctx.uc_link = NULL;

    t->state = READY;
    t->retval = NULL;
    t->waiting = NULL;

    makecontext(&t->ctx, (void (*)())thread_start, 2, func, arg);

    t->valgrind_stackid = VALGRIND_STACK_REGISTER(
        t->ctx.uc_stack.ss_sp,
        t->ctx.uc_stack.ss_sp + t->ctx.uc_stack.ss_size
    );

    sched_enqueue(t);

    *newthread = (thread_t)t;

    return 0;
}

int thread_yield(void) {
    if (sched_empty()) {
        return 0;
    }

    thread_m *prev = current;

    prev->state = READY;
    sched_enqueue(prev);

    thread_m *next = sched_dequeue();

    next->state = RUNNING;
    current = next;

    swapcontext(&prev->ctx, &next->ctx);

    return 0;
}

int thread_join(thread_t thread, void **retval) {
    thread_m *t = (thread_m *)thread;

    if (t->state != FINISHED) {
        t->waiting = current;
        current->state = BLOCKED;

        thread_m *next = sched_dequeue();
        if (!next) return -1;

        next->state = RUNNING;
        thread_m *prev = current;
        current = next;

        swapcontext(&prev->ctx, &next->ctx);
    }

    if (retval) {
        *retval = t->retval;
    }

    VALGRIND_STACK_DEREGISTER(t->valgrind_stackid);

    free(t->stack);
    free(t);

    return 0;
}

void thread_exit(void *retval) {
    current->retval = retval;

    current->state = FINISHED;

    if (current->waiting) {
        current->waiting->state = READY;
        sched_enqueue(current->waiting);
    }

    if (sched_empty()) {
        exit(0);
    }

    thread_m *next = sched_dequeue();
    next->state = RUNNING;
    current = next;

    setcontext(&next->ctx);

    exit(1);
}

int thread_mutex_init(thread_mutex_t *mutex){
    (void)mutex;
    return 0;
}

int thread_mutex_destroy(thread_mutex_t *mutex){
    (void)mutex;
    return 0;
}

int thread_mutex_lock(thread_mutex_t *mutex){
    (void)mutex;
    return 0;
}

int thread_mutex_unlock(thread_mutex_t *mutex){
    (void)mutex;
    return 0;
}

__attribute__((destructor)) static void cleanup_system(void) {
    if (current == NULL) return;

    while (!sched_empty()) {
        thread_m *t = sched_dequeue();
        if (t->stack != NULL) {
            VALGRIND_STACK_DEREGISTER(t->valgrind_stackid);
            free(t->stack);
        }
        free(t);
    }

    if (current != NULL) {
        free(current);
        current = NULL; 
    }
}