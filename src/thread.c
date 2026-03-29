#include "thread.h"
#include "scheduler.h"

#include <ucontext.h>
#include <stdlib.h>
#include <stdio.h>
#include <valgrind/valgrind.h>

#define STACK_SIZE 4096

static char exit_stack[4096];
static ucontext_t exit_ctx;

/*
Appelée quand le dernier thread (qui n'est pas le main) se termine.
Libère le thread courant, nettoie le scheduler et termine le programme.
Utilisé pour fixer la fuite mémoire du tests/12-join-main.c
*/
static void clean_exit(void)
{
    VALGRIND_STACK_DEREGISTER(current->valgrind_stackid);
    free(current->ctx.uc_stack.ss_sp);
    free(current);
    current = NULL;
    sched_cleanup();
    exit(0);
}

/*
Appelée automatiquement à la fin du programme (grâce à atexit).
Libère tous les threads restants notamment le thread main.
Utilisé pour nettoyer le thread main à la fin
*/
static void thread_system_destroy(void)
{
    while (!sched_empty())
    {
        thread_m *t = sched_dequeue();

        if (t->ctx.uc_stack.ss_sp != NULL)
        {
            VALGRIND_STACK_DEREGISTER(t->valgrind_stackid);
            free(t->ctx.uc_stack.ss_sp);
        }

        free(t);
    }
    if (current != NULL)
    {
        free(current);
        current = NULL;
    }

    sched_cleanup();
}

/*
Initialise le système de threads (crée un thread pour le main)
Appelle atexit pour nettoyer le système à la fin en appelant
la fonction "thread_system_destroy"
*/
static void init_system(void)
{
    sched_init();

    atexit(thread_system_destroy);

    current = malloc(sizeof(thread_m));
    if (!current)
    {
        perror("malloc main thread");
        exit(1);
    }
    getcontext(&current->ctx);
    current->state = RUNNING;
    current->ctx.uc_stack.ss_sp = NULL;
    current->retval = NULL;
    current->waiting = NULL;
}

/*
Fonction wrapper utilisée par makecontext.
Elle adapte la signature de la fonction et appelle thread_exit à la fin.
*/
static void thread_start(void *(*func)(void *), void *arg)
{
    void *ret = func(arg);
    thread_exit(ret);
}

thread_t thread_self(void)
{
    if (current == NULL)
    {
        init_system();
    }
    return (thread_t)current;
}

int thread_create(thread_t *newthread, void *(*func)(void *), void *arg)
{
    if (current == NULL)
    {
        init_system();
    }
    thread_m *t = malloc(sizeof(thread_m));
    if (!t)
        return -1;

    if (getcontext(&t->ctx) == -1)
    {
        free(t);
        return -1;
    }

    t->ctx.uc_stack.ss_sp = malloc(STACK_SIZE);
    t->ctx.uc_stack.ss_size = STACK_SIZE;
    t->ctx.uc_link = NULL;

    t->state = READY;
    t->retval = NULL;
    t->waiting = NULL;

    makecontext(&t->ctx, (void (*)())thread_start, 2, func, arg);

    t->valgrind_stackid = VALGRIND_STACK_REGISTER(
        t->ctx.uc_stack.ss_sp,
        t->ctx.uc_stack.ss_sp + t->ctx.uc_stack.ss_size);

    sched_enqueue(t);

    *newthread = (thread_t)t;

    return 0;
}

int thread_yield(void)
{
    if (current == NULL)
    {
        init_system();
    }

    if (sched_empty())
    {
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

int thread_join(thread_t thread, void **retval)
{
    thread_m *t = (thread_m *)thread;

    if (t->state != FINISHED)
    {
        t->waiting = current;
        current->state = BLOCKED;

        thread_m *next = sched_dequeue();
        if (!next)
            return -1;

        next->state = RUNNING;
        thread_m *prev = current;
        current = next;

        swapcontext(&prev->ctx, &next->ctx);
    }

    if (retval)
    {
        *retval = t->retval;
    }

    VALGRIND_STACK_DEREGISTER(t->valgrind_stackid);

    free(t->ctx.uc_stack.ss_sp);
    free(t);

    return 0;
}

void thread_exit(void *retval)
{
    current->retval = retval;

    current->state = FINISHED;

    if (current->waiting)
    {
        current->waiting->state = READY;
        sched_enqueue(current->waiting);
    }

    if (sched_empty())
    {
        getcontext(&exit_ctx);
        exit_ctx.uc_stack.ss_sp = exit_stack;
        exit_ctx.uc_stack.ss_size = sizeof(exit_stack);
        exit_ctx.uc_link = NULL;
        makecontext(&exit_ctx, clean_exit, 0);
        swapcontext(&current->ctx, &exit_ctx);
    }

    thread_m *next = sched_dequeue();
    next->state = RUNNING;
    current = next;

    setcontext(&next->ctx);

    exit(1);
}

int thread_mutex_init(thread_mutex_t *mutex)
{
    (void)mutex;
    return 0;
}

int thread_mutex_destroy(thread_mutex_t *mutex)
{
    (void)mutex;
    return 0;
}

int thread_mutex_lock(thread_mutex_t *mutex)
{
    (void)mutex;
    return 0;
}

int thread_mutex_unlock(thread_mutex_t *mutex)
{
    (void)mutex;
    return 0;
}
