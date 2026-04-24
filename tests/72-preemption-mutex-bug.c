#include "thread.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * Test : thread_mutex_lock fast path (mutex non contended)
 * ne doit pas laisser SIGALRM bloqué.
 *
 * Bug : preempt_block() est appelé en entrée de thread_mutex_lock,
 * mais le fast path (mutex libre) fait "return 0" sans preempt_restore().
 * Résultat : SIGALRM reste bloqué → la préemption est morte.
 *
 * Compiler avec -DUSE_PREEMPTION.
 */

static int sigalrm_is_blocked(void)
{
    sigset_t mask;
    sigprocmask(SIG_SETMASK, NULL, &mask);
    return sigismember(&mask, SIGALRM);
}

static volatile int worker_result = -1;

static void *worker(void *arg)
{
    thread_mutex_t *mutex = (thread_mutex_t *)arg;

    /* Ici le thread tourne sur sa propre stack, SIGALRM débloqué */
    if (sigalrm_is_blocked())
    {
        printf("ERREUR PREALABLE : SIGALRM bloqué avant mutex_lock\n");
        worker_result = 2;
        return NULL;
    }

    /* Lock sur un mutex LIBRE (fast path) */
    thread_mutex_lock(mutex);

    if (sigalrm_is_blocked())
    {
        printf("BUG CONFIRMÉ : SIGALRM bloqué après mutex_lock (fast path)\n");
        printf("La préemption est MORTE après un lock non-contended.\n");
        thread_mutex_unlock(mutex);
        worker_result = 1;
        return NULL;
    }

    printf("OK : SIGALRM non bloqué après mutex_lock fast path\n");
    thread_mutex_unlock(mutex);
    worker_result = 0;
    return NULL;
}

int main(void)
{
    thread_mutex_t mutex;
    thread_t th;

    thread_mutex_init(&mutex);
    thread_create(&th, worker, &mutex);

    /* Yield pour forcer le thread à tourner sur sa propre stack
     * (éviter le inline join qui masquerait le bug) */
    thread_yield();

    thread_join(th, NULL);
    thread_mutex_destroy(&mutex);

    if (worker_result == 0)
    {
        printf("SUCCÈS : mutex_lock fast path ne casse pas la préemption.\n");
        return EXIT_SUCCESS;
    }
    else
    {
        printf("ÉCHEC : le bug preempt_block sans restore est présent.\n");
        return EXIT_FAILURE;
    }
}
