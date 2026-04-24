/* Test multicore : mesure du speedup.
 * Travail CPU-bound (compter les nombres premiers jusqu'a K=50000).
 * Phase 1 : 1 thread sequentiel -> T_seq.
 * Phase 2 : N threads en parallele -> T_par.
 * Speedup = (N * T_seq) / T_par.
 *
 * support necessaire:
 * - thread_create()
 * - thread_join()
 */
#include "thread.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define PRIME_LIMIT 50000

struct padded_result {
    volatile long count;
    char pad[56];
};

static int is_prime(int n)
{
    if (n < 2) return 0;
    if (n < 4) return 1;
    if (n % 2 == 0) return 0;
    for (int i = 3; i * i <= n; i += 2)
        if (n % i == 0) return 0;
    return 1;
}

static int count_primes(int limit)
{
    int count = 0;
    for (int i = 2; i <= limit; i++)
        if (is_prime(i)) count++;
    return count;
}

static void *thfunc(void *arg)
{
    struct padded_result *res = (struct padded_result *)arg;
    res->count = count_primes(PRIME_LIMIT);
    return NULL;
}

static long timespec_us(struct timespec *start, struct timespec *end)
{
    return (end->tv_sec - start->tv_sec) * 1000000L
         + (end->tv_nsec - start->tv_nsec) / 1000;
}

int main(int argc, char *argv[])
{
    int nb = 4;
    if (argc >= 2)
        nb = atoi(argv[1]);
    if (nb <= 0) {
        fprintf(stderr, "nombre de threads invalide\n");
        return EXIT_FAILURE;
    }

    /* Phase 1: sequential */
    struct timespec t1, t2;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    int seq_result = count_primes(PRIME_LIMIT);
    clock_gettime(CLOCK_MONOTONIC, &t2);
    long t_seq = timespec_us(&t1, &t2);

    /* Phase 2: parallel */
    thread_t *th = malloc(nb * sizeof(*th));
    struct padded_result *results = calloc(nb, sizeof(*results));
    if (!th || !results) {
        perror("malloc");
        return EXIT_FAILURE;
    }

    int err;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    for (int i = 0; i < nb; i++) {
        err = thread_create(&th[i], thfunc, &results[i]);
        assert(!err);
    }
    for (int i = 0; i < nb; i++) {
        err = thread_join(th[i], NULL);
        assert(!err);
    }
    clock_gettime(CLOCK_MONOTONIC, &t2);
    long t_par = timespec_us(&t1, &t2);

    /* Verify all threads computed same result */
    int ok = 1;
    for (int i = 0; i < nb; i++) {
        if (results[i].count != seq_result) {
            printf("ERREUR: thread %d: %ld != %d\n", i, results[i].count, seq_result);
            ok = 0;
        }
    }

    free(th);
    free(results);

    if (!ok)
        return EXIT_FAILURE;

    double speedup = (double)nb * (double)t_seq / (double)(t_par > 0 ? t_par : 1);
    int speedup_pct = (int)(speedup * 100);

    printf("Sequential: %ld us, Parallel (%d threads): %ld us, Speedup: %.2fx\n",
           t_seq, nb, t_par, speedup);
    printf("GRAPH;121;%d;%ld;%d\n", nb, t_par, speedup_pct);
    return EXIT_SUCCESS;
}
