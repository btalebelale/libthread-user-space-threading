#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <sys/time.h>
#include "thread.h"

/*
 * Benchmark des coûts unitaires de chaque opération :
 *   1. create+join  : latence d'un cycle create/join
 *   2. yield        : débit de yields avec N threads
 *   3. join         : latence de join sur threads déjà terminés
 *   4. scalabilité  : yield throughput en fonction du nombre de threads
 */

#define N_CREATE    10000
#define N_YIELD     10000
#define N_YIELD_TH  50
#define N_JOIN      10000

static long elapsed_us(struct timeval *t1, struct timeval *t2)
{
    return (t2->tv_sec - t1->tv_sec) * 1000000L + (t2->tv_usec - t1->tv_usec);
}

/* ── 1. create+join latency ────────────────────────────── */

static void *noop_func(void *arg __attribute__((unused)))
{
    thread_exit(NULL);
    return NULL;
}

static void bench_create_join(void)
{
    struct timeval t1, t2;
    thread_t th;
    int err;

    gettimeofday(&t1, NULL);
    for (int i = 0; i < N_CREATE; i++) {
        err = thread_create(&th, noop_func, NULL);
        assert(!err);
        err = thread_join(th, NULL);
        assert(!err);
    }
    gettimeofday(&t2, NULL);

    long us = elapsed_us(&t1, &t2);
    printf("  create+join  : %6.2f us/op  (%d ops, %ld us total)\n",
           (double)us / N_CREATE, N_CREATE, us);
}

/* ── 2. yield throughput ───────────────────────────────── */

struct yield_arg {
    int n_yields;
};

static void *yield_func(void *arg)
{
    struct yield_arg *ya = arg;
    for (int i = 0; i < ya->n_yields; i++)
        thread_yield();
    return NULL;
}

static void bench_yield(int n_threads, int n_yields)
{
    struct timeval t1, t2;
    thread_t *threads = malloc(sizeof(thread_t) * n_threads);
    assert(threads);

    struct yield_arg ya = { .n_yields = n_yields };

    gettimeofday(&t1, NULL);

    for (int i = 0; i < n_threads; i++) {
        int err = thread_create(&threads[i], yield_func, &ya);
        assert(!err);
    }

    /* main participe aussi aux yields */
    for (int i = 0; i < n_yields; i++)
        thread_yield();

    for (int i = 0; i < n_threads; i++) {
        int err = thread_join(threads[i], NULL);
        assert(!err);
    }

    gettimeofday(&t2, NULL);

    long us = elapsed_us(&t1, &t2);
    long total_yields = (long)(n_threads + 1) * n_yields;
    printf("  yield        : %6.2f us/op  (%ld yields, %d threads, %ld us total)\n",
           (double)us / total_yields, total_yields, n_threads, us);

    free(threads);
}

/* ── 3. join latency (threads déjà terminés) ───────────── */

static void bench_join(void)
{
    struct timeval t1, t2;
    thread_t *threads = malloc(sizeof(thread_t) * N_JOIN);
    assert(threads);

    /* Créer tous les threads d'abord */
    for (int i = 0; i < N_JOIN; i++) {
        int err = thread_create(&threads[i], noop_func, NULL);
        assert(!err);
    }

    /* Yield pour laisser les threads se terminer */
    for (int i = 0; i < N_JOIN; i++)
        thread_yield();

    /* Mesurer le coût du join seul */
    gettimeofday(&t1, NULL);
    for (int i = 0; i < N_JOIN; i++) {
        int err = thread_join(threads[i], NULL);
        assert(!err);
    }
    gettimeofday(&t2, NULL);

    long us = elapsed_us(&t1, &t2);
    printf("  join         : %6.2f us/op  (%d ops, %ld us total)\n",
           (double)us / N_JOIN, N_JOIN, us);

    free(threads);
}

/* ── 4. scalabilité yield ──────────────────────────────── */

static void bench_yield_scalability(void)
{
    int thread_counts[] = {1, 2, 5, 10, 25, 50, 100, 200, 500};
    int n_counts = sizeof(thread_counts) / sizeof(thread_counts[0]);
    int yields_per_thread = 5000;

    printf("\n=== Scalabilité yield (%d yields/thread) ===\n", yields_per_thread);

    for (int c = 0; c < n_counts; c++) {
        int nth = thread_counts[c];
        if (nth > 1000) break; /* ne pas dépasser SCHED_MAX_THREADS */

        struct timeval t1, t2;
        thread_t *threads = malloc(sizeof(thread_t) * nth);
        assert(threads);

        struct yield_arg ya = { .n_yields = yields_per_thread };

        gettimeofday(&t1, NULL);

        for (int i = 0; i < nth; i++) {
            int err = thread_create(&threads[i], yield_func, &ya);
            assert(!err);
        }

        for (int i = 0; i < yields_per_thread; i++)
            thread_yield();

        for (int i = 0; i < nth; i++) {
            int err = thread_join(threads[i], NULL);
            assert(!err);
        }

        gettimeofday(&t2, NULL);

        long us = elapsed_us(&t1, &t2);
        long total_yields = (long)(nth + 1) * yields_per_thread;
        printf("  %3d threads  : %6.2f us/yield  (%ld us total)\n",
               nth, (double)us / total_yields, us);

        free(threads);
    }
}

/* ── 5. scalabilité création ───────────────────────────── */

static void bench_create_scalability(void)
{
    int counts[] = {100, 500, 1000, 5000, 10000, 50000};
    int n_counts = sizeof(counts) / sizeof(counts[0]);

    printf("\n=== Scalabilité création (create+join séquentiel) ===\n");

    for (int c = 0; c < n_counts; c++) {
        int n = counts[c];
        struct timeval t1, t2;
        thread_t th;

        gettimeofday(&t1, NULL);
        for (int i = 0; i < n; i++) {
            int err = thread_create(&th, noop_func, NULL);
            assert(!err);
            err = thread_join(th, NULL);
            assert(!err);
        }
        gettimeofday(&t2, NULL);

        long us = elapsed_us(&t1, &t2);
        printf("  %5d threads : %6.2f us/op  (%ld us total)\n",
               n, (double)us / n, us);
    }
}

/* ── main ──────────────────────────────────────────────── */

int main(void)
{
    printf("=== Benchmark : coûts unitaires ===\n");
    bench_create_join();
    bench_yield(N_YIELD_TH, N_YIELD);
    bench_join();

    bench_yield_scalability();
    bench_create_scalability();

    return 0;
}
