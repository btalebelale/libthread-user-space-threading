/* Test multicore : stress mutex haute contention.
 * N threads, 4 mutex, I iterations. Thread i prend mutex[i%4],
 * incremente counter[i%4], relache. Verifie sum(counters) == N * I.
 *
 * support necessaire:
 * - thread_create()
 * - thread_join()
 * - thread_mutex_init/lock/unlock/destroy()
 */
#include "thread.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

#define N_MUTEXES 4

static thread_mutex_t mutexes[N_MUTEXES];
static int counters[N_MUTEXES];

struct thread_arg {
    int id;
    int iters;
};

static void *thfunc(void *arg)
{
    struct thread_arg *ta = (struct thread_arg *)arg;
    int idx = ta->id % N_MUTEXES;
    for (int i = 0; i < ta->iters; i++) {
        thread_mutex_lock(&mutexes[idx]);
        counters[idx]++;
        thread_mutex_unlock(&mutexes[idx]);
    }
    return NULL;
}

int main(int argc, char *argv[])
{
    int nb = 8;
    int iters = 100000;
    if (argc >= 2)
        nb = atoi(argv[1]);
    if (argc >= 3)
        iters = atoi(argv[2]);
    if (nb <= 0 || iters <= 0) {
        fprintf(stderr, "parametres invalides\n");
        return EXIT_FAILURE;
    }

    for (int i = 0; i < N_MUTEXES; i++)
        thread_mutex_init(&mutexes[i]);

    thread_t *th = malloc(nb * sizeof(*th));
    struct thread_arg *args = malloc(nb * sizeof(*args));
    if (!th || !args) {
        perror("malloc");
        return EXIT_FAILURE;
    }

    struct timeval tv1, tv2;
    gettimeofday(&tv1, NULL);

    int err;
    for (int i = 0; i < nb; i++) {
        args[i].id = i;
        args[i].iters = iters;
        err = thread_create(&th[i], thfunc, &args[i]);
        assert(!err);
    }

    for (int i = 0; i < nb; i++) {
        err = thread_join(th[i], NULL);
        assert(!err);
    }

    gettimeofday(&tv2, NULL);

    for (int i = 0; i < N_MUTEXES; i++)
        thread_mutex_destroy(&mutexes[i]);

    unsigned long us = (tv2.tv_sec - tv1.tv_sec) * 1000000
                     + (tv2.tv_usec - tv1.tv_usec);

    long total = 0;
    for (int i = 0; i < N_MUTEXES; i++)
        total += counters[i];

    free(th);
    free(args);

    long expected = (long)nb * iters;
    if (total == expected) {
        printf("Stress mutex OK: %d threads * %d iters = %ld ops en %lu us\n",
               nb, iters, expected, us);
        return EXIT_SUCCESS;
    } else {
        printf("ERREUR: attendu %ld, obtenu %ld\n", expected, total);
        return EXIT_FAILURE;
    }
}
