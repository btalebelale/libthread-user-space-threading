/* Test multicore : stress create/join.
 * W workers, chacun fait C cycles de: create un child -> join.
 * Le child retourne (void*)(intptr_t)(worker_id * cycles + cycle).
 * Verifie la valeur de retour a chaque cycle.
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

static int cycles_per_worker;

static void *child_func(void *arg)
{
    return arg;
}

struct worker_arg {
    int worker_id;
    int ok;
};

static void *worker_func(void *arg)
{
    struct worker_arg *wa = (struct worker_arg *)arg;
    wa->ok = 1;

    for (int c = 0; c < cycles_per_worker; c++) {
        intptr_t expected = (intptr_t)wa->worker_id * cycles_per_worker + c;
        thread_t child;
        int cerr = thread_create(&child, child_func, (void *)expected);
        assert(!cerr);

        void *retval;
        cerr = thread_join(child, &retval);
        assert(!cerr);

        if ((intptr_t)retval != expected) {
            fprintf(stderr, "ERREUR: worker %d cycle %d: attendu %ld, obtenu %ld\n",
                    wa->worker_id, c, (long)expected, (long)(intptr_t)retval);
            wa->ok = 0;
            return NULL;
        }
    }
    return NULL;
}

int main(int argc, char *argv[])
{
    int nb_workers = 4;
    cycles_per_worker = 10000;
    if (argc >= 2)
        nb_workers = atoi(argv[1]);
    if (argc >= 3)
        cycles_per_worker = atoi(argv[2]);
    if (nb_workers <= 0 || cycles_per_worker <= 0) {
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

    long total = (long)nb_workers * cycles_per_worker;

    free(th);
    free(args);

    if (all_ok) {
        printf("Stress create/join OK: %d workers * %d cycles = %ld create/joins en %lu us\n",
               nb_workers, cycles_per_worker, total, us);
        return EXIT_SUCCESS;
    } else {
        printf("ERREUR: retval mismatch detecte\n");
        return EXIT_FAILURE;
    }
}
