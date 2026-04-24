/* Test multicore : equite du yield.
 * N threads font du yield en boucle pendant 500ms, chacun incremente
 * son propre compteur (padde pour eviter le false sharing).
 * Verifie que le ratio min/max >= 0.3.
 *
 * support necessaire:
 * - thread_create()
 * - thread_join()
 * - thread_yield()
 */
#include "thread.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

#define DURATION_US 500000 /* 500 ms */

struct padded_counter {
    volatile long count;
    char pad[56];
};

static struct timeval start;

static long elapsed_us(void)
{
    struct timeval now;
    gettimeofday(&now, NULL);
    return (now.tv_sec - start.tv_sec) * 1000000L
         + (now.tv_usec - start.tv_usec);
}

static void *thfunc(void *arg)
{
    struct padded_counter *ctr = (struct padded_counter *)arg;
    while (elapsed_us() < DURATION_US) {
        thread_yield();
        ctr->count++;
    }
    return NULL;
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
    struct padded_counter *ctrs = calloc(nb, sizeof(*ctrs));
    if (!th || !ctrs) {
        perror("malloc");
        return EXIT_FAILURE;
    }

    gettimeofday(&start, NULL);

    int err;
    for (int i = 0; i < nb; i++) {
        err = thread_create(&th[i], thfunc, &ctrs[i]);
        assert(!err);
    }

    for (int i = 0; i < nb; i++) {
        err = thread_join(th[i], NULL);
        assert(!err);
    }

    long min_count = ctrs[0].count;
    long max_count = ctrs[0].count;
    for (int i = 0; i < nb; i++) {
        printf("  thread %d: %ld yields\n", i, ctrs[i].count);
        if (ctrs[i].count < min_count)
            min_count = ctrs[i].count;
        if (ctrs[i].count > max_count)
            max_count = ctrs[i].count;
    }

    free(th);
    free(ctrs);

    if (max_count == 0) {
        printf("ERREUR: aucun yield effectue\n");
        return EXIT_FAILURE;
    }

    double ratio = (double)min_count / (double)max_count;
    printf("min=%ld max=%ld ratio=%.3f\n", min_count, max_count, ratio);

    if (ratio < 0.3) {
        printf("ERREUR: equite insuffisante (ratio %.3f < 0.3)\n", ratio);
        return EXIT_FAILURE;
    }

    printf("Equite yield OK: %d threads, ratio %.3f\n", nb, ratio);
    return EXIT_SUCCESS;
}
