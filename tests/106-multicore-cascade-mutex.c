#include "thread.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

/* Test de synchronisation en pipeline mutex+sémaphore.
 *
 * 8 étages de pipeline, 1000 rounds. Chaque étage i attend sur sem[i],
 * prend le mutex, ajoute i au résultat partagé, libère le mutex, puis
 * poste sem[i+1]. Le thread principal lance chaque round en postant
 * sem[0] et attend la fin du round sur sem[N_STAGES].
 *
 * Résultat attendu : 1000 * (0+1+2+3+4+5+6+7) = 28000
 */

#define N_STAGES 8
#define N_ROUNDS 1000

static thread_sem_t sems[N_STAGES + 1];
static thread_mutex_t mutex;
static long result = 0;

struct stage_arg {
    int stage_id;
};

static void *stage_func(void *arg)
{
    struct stage_arg *sa = (struct stage_arg *)arg;
    int id = sa->stage_id;

    for (int r = 0; r < N_ROUNDS; r++) {
        thread_sem_wait(&sems[id]);
        thread_mutex_lock(&mutex);
        result += id;
        thread_mutex_unlock(&mutex);
        thread_sem_post(&sems[id + 1]);
    }

    return NULL;
}

int main(void)
{
    thread_mutex_init(&mutex);

    int err;
    for (int i = 0; i <= N_STAGES; i++) {
        err = thread_sem_init(&sems[i], 0);
        assert(!err);
    }

    struct stage_arg args[N_STAGES];
    thread_t threads[N_STAGES];

    for (int i = 0; i < N_STAGES; i++) {
        args[i].stage_id = i;
        err = thread_create(&threads[i], stage_func, &args[i]);
        assert(!err);
    }

    /* Drive the pipeline: post sem[0] to start each round,
     * wait on sem[N_STAGES] for completion */
    for (int r = 0; r < N_ROUNDS; r++) {
        thread_sem_post(&sems[0]);
        thread_sem_wait(&sems[N_STAGES]);
    }

    for (int i = 0; i < N_STAGES; i++) {
        err = thread_join(threads[i], NULL);
        assert(!err);
    }

    for (int i = 0; i <= N_STAGES; i++)
        thread_sem_destroy(&sems[i]);
    thread_mutex_destroy(&mutex);

    long expected = (long)N_ROUNDS * (N_STAGES * (N_STAGES - 1) / 2);
    if (result != expected) {
        printf("FAIL: result = %ld, expected %ld\n", result, expected);
        return EXIT_FAILURE;
    }

    printf("cascade mutex+sem OK: %d stages x %d rounds, result = %ld\n",
           N_STAGES, N_ROUNDS, result);
    return EXIT_SUCCESS;
}
