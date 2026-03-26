#include "thread.h"
#include <ucontext.h>
#include <stdlib.h>
#include <stdio.h> 
#include <sys/queue.h>

#define STACK_SIZE 65536

STAILQ_HEAD(thread_queue, thread);

typedef enum {
    READY,
    RUNNING,
    BLOCKED,
    FINISHED
} state_t;

typedef struct thread {
    ucontext_t ctx;
    void *stack;

    void *retval;
    state_t state;

    STAILQ_ENTRY(thread) link;
    struct thread *waiting;
} thread_m;

static thread_m *current = NULL;
static struct thread_queue ready_queue;

thread_t thread_self(void) {
    return (thread_t) current;
}

void thread_start(void *(*func)(void *), void *arg) {
    void *ret = func(arg);
    thread_exit(ret);
}

int thread_create(thread_t *newthread, void *(*func)(void *), void *arg) {
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

    STAILQ_INSERT_TAIL(&ready_queue, t, link);

    *newthread = (thread_t)t;

    return 0;
}

int thread_yield(void) {
    if (STAILQ_EMPTY(&ready_queue)) {
        return 0;
    }

    thread_m *prev = current;

    prev->state = READY;
    STAILQ_INSERT_TAIL(&ready_queue, prev, link);

    thread_m *next = STAILQ_FIRST(&ready_queue);
    STAILQ_REMOVE_HEAD(&ready_queue, link);

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

        thread_m *next = STAILQ_FIRST(&ready_queue);
        if (!next) return -1;

        STAILQ_REMOVE_HEAD(&ready_queue, link);

        next->state = RUNNING;
        thread_m *prev = current;
        current = next;

        swapcontext(&prev->ctx, &next->ctx);
    }

    if (retval) {
        *retval = t->retval;
    }

    free(t->stack);
    free(t);

    return 0;
}

void thread_exit(void *retval) {
    current->retval = retval;

    current->state = FINISHED;

    if (current->waiting) {
        current->waiting->state = READY;
        STAILQ_INSERT_TAIL(&ready_queue, current->waiting, link);
    }

    if (STAILQ_EMPTY(&ready_queue)) {
        exit(0);
    }

    thread_m *next = STAILQ_FIRST(&ready_queue);
    STAILQ_REMOVE_HEAD(&ready_queue, link);

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