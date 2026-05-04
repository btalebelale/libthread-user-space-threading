/*
 * bench_functions.c — Micro-benchmark de chaque fonction de la bibliothèque de threads.
 *
 * Compile avec :
 *   gcc -O2 -Wall -I./src bench_functions.c -o bench_functions -L. -lthread
 * Lance avec :
 *   LD_LIBRARY_PATH=. ./bench_functions
 *
 * Mesure le temps moyen de chaque fonction de l'API thread.h
 */

#include "thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* ---------- helpers ---------- */

static inline long long now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

#define BENCH(label, ITERATIONS, SETUP, BODY, TEARDOWN)                       \
    do {                                                                       \
        SETUP;                                                                 \
        long long _start = now_ns();                                           \
        for (int _i = 0; _i < (ITERATIONS); _i++) { BODY; }                   \
        long long _end = now_ns();                                             \
        TEARDOWN;                                                              \
        double _avg = (double)(_end - _start) / (ITERATIONS);                  \
        printf("%-30s  %8d iter  %10.1f ns/op\n", label, (ITERATIONS), _avg); \
    } while (0)

/* ---------- thread functions for benchmarks ---------- */

static void *noop_func(void *arg)
{
    (void)arg;
    return NULL;
}

static void *yield_func(void *arg)
{
    int n = *(int *)arg;
    for (int i = 0; i < n; i++)
        thread_yield();
    return NULL;
}

static void *exit_func(void *arg)
{
    (void)arg;
    thread_exit(NULL);
    /* unreachable */
}

/* ---------- benchmarks ---------- */

static void bench_thread_self(void)
{
    BENCH("thread_self", 1000000,
        {},
        { thread_self(); },
        {});
}

static void bench_create_join(void)
{
    int iters = 100000;
    BENCH("thread_create+join", iters,
        {},
        {
            thread_t th;
            thread_create(&th, noop_func, NULL);
            thread_join(th, NULL);
        },
        {});
}

static void bench_create_exit_join(void)
{
    int iters = 100000;
    BENCH("thread_create+exit+join", iters,
        {},
        {
            thread_t th;
            thread_create(&th, exit_func, NULL);
            thread_join(th, NULL);
        },
        {});
}

static void bench_yield_solo(void)
{
    /* yield quand on est le seul thread : mesure le coût minimal */
    BENCH("thread_yield (solo)", 1000000,
        {},
        { thread_yield(); },
        {});
}

static void bench_yield_pair(void)
{
    /* yield entre 2 threads */
    int total_yields = 500000;
    int per_thread = total_yields;

    long long start = now_ns();

    thread_t th;
    thread_create(&th, yield_func, &per_thread);
    for (int i = 0; i < per_thread; i++)
        thread_yield();
    thread_join(th, NULL);

    long long end = now_ns();
    /* total context switches = ~2 * per_thread (each yield from main wakes child and vice versa) */
    double avg = (double)(end - start) / (2.0 * per_thread);
    printf("%-30s  %8d iter  %10.1f ns/op\n", "thread_yield (2 threads)", 2 * per_thread, avg);
}

static void bench_mutex(void)
{
    thread_mutex_t m;
    BENCH("mutex lock+unlock (no contention)", 1000000,
        { thread_mutex_init(&m); },
        {
            thread_mutex_lock(&m);
            thread_mutex_unlock(&m);
        },
        { thread_mutex_destroy(&m); });
}

static void bench_mutex_init_destroy(void)
{
    BENCH("mutex init+destroy", 1000000,
        {},
        {
            thread_mutex_t m;
            thread_mutex_init(&m);
            thread_mutex_destroy(&m);
        },
        {});
}

static void bench_sem(void)
{
    thread_sem_t s;
    BENCH("sem wait+post (no contention)", 1000000,
        { thread_sem_init(&s, 1); },
        {
            thread_sem_wait(&s);
            thread_sem_post(&s);
        },
        { thread_sem_destroy(&s); });
}

static void bench_sem_init_destroy(void)
{
    BENCH("sem init+destroy", 1000000,
        {},
        {
            thread_sem_t s;
            thread_sem_init(&s, 1);
            thread_sem_destroy(&s);
        },
        {});
}

/* ---------- main ---------- */

int main(void)
{
    printf("=== Benchmark des fonctions de la bibliotheque de threads ===\n");
    printf("%-30s  %8s  %12s\n", "Fonction", "Iters", "Temps/op");
    printf("--------------------------------------------------------------\n");

    bench_thread_self();
    bench_create_join();
    bench_create_exit_join();
    bench_yield_solo();
    bench_yield_pair();
    bench_mutex_init_destroy();
    bench_mutex();
    bench_sem_init_destroy();
    bench_sem();

    printf("--------------------------------------------------------------\n");
    return 0;
}
