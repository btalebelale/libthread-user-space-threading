#include "thread.h"
#include "scheduler.h"

#include <stdio.h>
#include <stdlib.h>
#include <valgrind/valgrind.h>

#define STACK_SIZE (16 * 1024)

static char exit_stack[STACK_SIZE];

/*
Appelée quand le dernier thread (qui n'est pas le main) se termine.
Libère le thread courant, nettoie le scheduler et termine le programme.
Utilisé pour fixer la fuite mémoire du tests/12-join-main.c
*/
static void clean_exit(void)
{
    VALGRIND_STACK_DEREGISTER(current->valgrind_stackid);
    free(current->stack);
    free(current);
    current = NULL;
    exit(0);
}

/*
Appelée automatiquement à la fin du programme (grâce à atexit).
Libère tous les threads restants notamment le thread main.
Utilisé pour nettoyer le thread main à la fin
*/
static void thread_system_destroy(void)
{
    while (!is_sched_empty())
    {
        thread_m *t = sched_dequeue();

        if (t->stack != NULL)
        {
            VALGRIND_STACK_DEREGISTER(t->valgrind_stackid);
            free(t->stack);
        }

        free(t);
    }
    if (current != NULL)
    {
        free(current);
        current = NULL;
    }
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
    setjmp(current->env);
    current->state = RUNNING;
    current->stack_size = 0;
    current->stack = NULL;
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
    {
        return -1;
    }

    t->stack_size = STACK_SIZE;
    t->stack = malloc(t->stack_size);
    if (!t->stack)
    {
        return -1;
    }

    t->state = READY;
    t->retval = NULL;
    t->waiting = NULL;
    t->func = func;
    t->func_arg = arg;

    t->valgrind_stackid = VALGRIND_STACK_REGISTER(t->stack, t->stack + t->stack_size);

    void *old_rsp = NULL;
    void *top = t->stack + t->stack_size;

    asm("mov %%rsp, %0" : "=r"(old_rsp));
    asm("mov %0, %%rsp" : : "r"(top));

    if (setjmp(t->env) == 0)
    {
        asm("mov %0, %%rsp" : : "r"(old_rsp));
    }
    else
    {
        thread_start(current->func, current->func_arg);
    }

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

    if (is_sched_empty())
    {
        return 0;
    }

    thread_m *prev = current;

    prev->state = READY;
    sched_enqueue(prev);

    thread_m *next = sched_dequeue();

    next->state = RUNNING;
    current = next;

    if (setjmp(prev->env) == 0)
    {
        longjmp(next->env, 1);
    }

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

        if (setjmp(prev->env) == 0)
        {
            longjmp(next->env, 1);
        }
    }

    if (retval)
    {
        *retval = t->retval;
    }

    VALGRIND_STACK_DEREGISTER(t->valgrind_stackid);

    free(t->stack);
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

    if (is_sched_empty())
    {

        void *top = exit_stack + sizeof(exit_stack);
        asm("mov %0, %%rsp" : : "r"(top));
        clean_exit();
    }

    thread_m *next = sched_dequeue();
    next->state = RUNNING;
    current = next;

    longjmp(next->env, 1);

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
