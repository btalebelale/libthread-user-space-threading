/* Test multicore : stress memoire (create/destroy en batch).
 * W workers, chacun fait R rounds de: creer B threads -> joindre les B.
 * Chaque thread enfant fait un petit calcul et retourne un resultat verifiable.
 *
 * support necessaire:
 * - thread_create()
 * - thread_join() avec recuperation de la valeur de retour
 */
#include "thread.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/time.h>

static void *child_func(void *arg)
{
    unsigned long val = (unsigned long)(uintptr_t)arg;
    val = val * 6364136223846793005UL + 1;
    return (void *)(uintptr_t)val;
}

struct worker_arg {
    int worker_id;
    int rounds;
    int batch;
    int ok;
};

static void *worker_func(void *arg)
{
    struct worker_arg *wa = (struct worker_arg *)arg;
    wa->ok = 1;

    thread_t *children = malloc(wa->batch * sizeof(*children));
    if (!children) {
        wa->ok = 0;
        return NULL;
    }

    int err;
    for (int r = 0; r < wa->rounds; r++) {
        for (int b = 0; b < wa->batch; b++) {
            unsigned long seed = (unsigned long)wa->worker_id * 1000000
                               + (unsigned long)r * 1000 + (unsigned long)b;
            err = thread_create(&children[b], child_func, (void *)(uintptr_t)seed);
            assert(!err);
        }
        for (int b = 0; b < wa->batch; b++) {
            void *retval;
            err = thread_join(children[b], &retval);
            assert(!err);
            unsigned long seed = (unsigned long)wa->worker_id * 1000000
                               + (unsigned long)r * 1000 + (unsigned long)b;
            unsigned long expected = seed * 6364136223846793005UL + 1;
            if ((uintptr_t)retval != (uintptr_t)expected) {
                wa->ok = 0;
            }
        }
    }

    free(children);
    return NULL;
}

int main(int argc, char *argv[])
{
    int nb_workers = 4;
    int rounds = 100;
    int batch = 50;
    if (argc >= 2)
        nb_workers = atoi(argv[1]);
    if (argc >= 3)
        rounds = atoi(argv[2]);
    if (argc >= 4)
        batch = atoi(argv[3]);
    if (nb_workers <= 0 || rounds <= 0 || batch <= 0) {
        fprintf(stderr, "parametres invalides\n");
        return EXIT_FAILURE;
    }

    thread_t *th = malloc(nb_workers * sizeof(*th));
    struct worker_arg *args = malloc(nb_workers * sizeof(*args));
    if (!th || !args) {
        perror("malloc");
        return EXIT_FAILURE;
    }

    struct timeval tv1, tv2;
    gettimeofday(&tv1, NULL);

    int err;
    for (int i = 0; i < nb_workers; i++) {
        args[i].worker_id = i;
        args[i].rounds = rounds;
        args[i].batch = batch;
        args[i].ok = 0;
        err = thread_create(&th[i], worker_func, &args[i]);
        assert(!err);
    }

    int all_ok = 1;
    for (int i = 0; i < nb_workers; i++) {
        err = thread_join(th[i], NULL);
        assert(!err);
        if (!args[i].ok)
            all_ok = 0;
    }

    gettimeofday(&tv2, NULL);

    unsigned long us = (tv2.tv_sec - tv1.tv_sec) * 1000000
                     + (tv2.tv_usec - tv1.tv_usec);
    long total = (long)nb_workers * rounds * batch;

    free(th);
    free(args);

    if (all_ok) {
        printf("Memory stress OK: %d workers * %d rounds * %d batch = %ld threads en %lu us\n",
               nb_workers, rounds, batch, total, us);
        return EXIT_SUCCESS;
    } else {
        printf("ERREUR: retval mismatch detecte\n");
        return EXIT_FAILURE;
    }
}
