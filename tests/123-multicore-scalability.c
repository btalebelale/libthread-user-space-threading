/* Test multicore : scalabilite de l'ordonnanceur.
 * Travail total constant. Pour N dans {1, 2, 4, 8, 16, 32} :
 * creer N threads, chacun fait TOTAL_WORK/N unites de calcul.
 * Mesure le temps et l'efficacite.
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

#define TOTAL_WORK 10000000L

struct padded_arg {
    long iters;
    volatile unsigned long result;
    char pad[48];
};

static void *thfunc(void *arg)
{
    struct padded_arg *pa = (struct padded_arg *)arg;
    unsigned long x = 0x12345;
    for (long i = 0; i < pa->iters; i++)
        x = x * 6364136223846793005UL + 1;
    pa->result = x;
    return NULL;
}

static long timespec_us(struct timespec *start, struct timespec *end)
{
    return (end->tv_sec - start->tv_sec) * 1000000L
         + (end->tv_nsec - start->tv_nsec) / 1000;
}

int main(void)
{
    int thread_counts[] = {1, 2, 4, 8, 16, 32};
    int n_configs = 6;
    long t1_time = 0;

    printf("%-5s %10s %10s\n", "N", "time_us", "efficiency");
    printf("----- ---------- ----------\n");

    for (int c = 0; c < n_configs; c++) {
        int nt = thread_counts[c];
        long iters_per = TOTAL_WORK / nt;

        thread_t *th = malloc(nt * sizeof(*th));
        struct padded_arg *args = calloc(nt, sizeof(*args));
        if (!th || !args) {
            perror("malloc");
            return EXIT_FAILURE;
        }

        struct timespec ts1, ts2;
        int err;

        clock_gettime(CLOCK_MONOTONIC, &ts1);
        for (int i = 0; i < nt; i++) {
            args[i].iters = iters_per;
            err = thread_create(&th[i], thfunc, &args[i]);
            assert(!err);
        }
        for (int i = 0; i < nt; i++) {
            err = thread_join(th[i], NULL);
            assert(!err);
        }
        clock_gettime(CLOCK_MONOTONIC, &ts2);

        long us = timespec_us(&ts1, &ts2);
        if (c == 0)
            t1_time = us;

        double efficiency = (t1_time > 0 && us > 0)
                          ? 100.0 * (double)t1_time / ((double)nt * (double)us)
                          : 0.0;

        printf("N=%2d  %10ld    %5.1f%%\n", nt, us, efficiency);
        printf("GRAPH;123;%d;%ld\n", nt, us);

        free(th);
        free(args);
    }

    return EXIT_SUCCESS;
}
