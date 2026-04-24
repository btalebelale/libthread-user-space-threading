/* Test multicore : integrite des registres.
 * N threads calculent chacun un hash deterministe sur 1M iterations
 * avec de nombreuses variables locales pour stresser les registres.
 * Le main recalcule les valeurs attendues et compare.
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

static unsigned long compute_hash(unsigned long seed)
{
    unsigned long a = seed, b = seed ^ 0xFF00FF00FF00FF00UL;
    unsigned long c = seed << 17, d = seed >> 13;
    for (int i = 0; i < 1000000; i++) {
        a = a * 6364136223846793005UL + 1;
        b ^= a >> 17;
        c += b * 2862933555777941757UL;
        d ^= c << 23;
        a += d >> 7;
    }
    return a ^ b ^ c ^ d;
}

static void *thfunc(void *arg)
{
    unsigned long seed = (unsigned long)(uintptr_t)arg;
    unsigned long result = compute_hash(seed);
    return (void *)(uintptr_t)result;
}

int main(int argc, char *argv[])
{
    int nb = 8;
    if (argc >= 2)
        nb = atoi(argv[1]);
    if (nb <= 0) {
        fprintf(stderr, "nombre de threads invalide\n");
        return EXIT_FAILURE;
    }

    thread_t *th = malloc(nb * sizeof(*th));
    if (!th) {
        perror("malloc");
        return EXIT_FAILURE;
    }

    int err;
    struct timeval tv1, tv2;
    gettimeofday(&tv1, NULL);

    for (int i = 0; i < nb; i++) {
        unsigned long seed = (unsigned long)(i + 1) * 0xDEADBEEFCAFE0001UL;
        err = thread_create(&th[i], thfunc, (void *)(uintptr_t)seed);
        assert(!err);
    }

    int ok = 1;
    for (int i = 0; i < nb; i++) {
        void *retval;
        err = thread_join(th[i], &retval);
        assert(!err);

        unsigned long seed = (unsigned long)(i + 1) * 0xDEADBEEFCAFE0001UL;
        unsigned long expected = compute_hash(seed);
        unsigned long got = (unsigned long)(uintptr_t)retval;

        if (got != expected) {
            printf("ERREUR thread %d: attendu %lx, obtenu %lx\n",
                   i, expected, got);
            ok = 0;
        }
    }

    gettimeofday(&tv2, NULL);
    free(th);

    unsigned long us = (tv2.tv_sec - tv1.tv_sec) * 1000000
                     + (tv2.tv_usec - tv1.tv_usec);

    if (ok) {
        printf("Integrite registres OK: %d threads en %lu us\n", nb, us);
        printf("GRAPH;102;%d;%lu\n", nb, us);
        return EXIT_SUCCESS;
    } else {
        printf("ERREUR: corruption detectee\n");
        return EXIT_FAILURE;
    }
}
