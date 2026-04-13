#define _GNU_SOURCE
#include "thread.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static inline uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static void *worker_a(void *arg)
{
    (void)arg;
    return NULL;
}
static void *worker_b(void *arg)
{
    (void)arg;
    return NULL;
}
static void *worker_c(void *arg)
{
    (void)arg;
    return NULL;
}
static void *worker_d(void *arg)
{
    (void)arg;
    return NULL;
}
static void *(*workers[])(void *) = {worker_a, worker_b, worker_c, worker_d};

// Dup hit : même (func, arg) N fois
static double bench_dup_hit(int n)
{
    thread_t first, dup;
    thread_create(&first, worker_a, (void *)42);
    uint64_t t0 = now_ns();
    for (int i = 0; i < n; i++)
        thread_create(&dup, worker_a, (void *)42);
    uint64_t t1 = now_ns();
    thread_join(first, NULL);
    return (double)(t1 - t0) / n;
}

// Cycle create+join
static double bench_cycle(int n)
{
    thread_t t;
    uint64_t t0 = now_ns();
    for (int i = 0; i < n; i++)
    {
        thread_create(&t, worker_a, (void *)(intptr_t)i);
        thread_join(t, NULL);
    }
    uint64_t t1 = now_ns();
    return (double)(t1 - t0) / n;
}

// Miss sous charge
static double bench_miss_loaded(int active, int lookups)
{
    thread_t *threads = malloc((active + lookups) * sizeof(thread_t));
    for (int i = 0; i < active; i++)
        thread_create(&threads[i], workers[i & 3], (void *)(intptr_t)(i + 1000000));

    uint64_t t0 = now_ns();
    for (int i = 0; i < lookups; i++)
        thread_create(&threads[active + i], worker_a, (void *)(intptr_t)(-(i + 1)));
    uint64_t t1 = now_ns();

    for (int i = 0; i < active + lookups; i++)
        thread_join(threads[i], NULL);
    free(threads);
    return (double)(t1 - t0) / lookups;
}

int main(void)
{
#ifdef DEDUP_SWISS
    const char *impl = "swiss";
#elif defined(DEDUP_AOS)
    const char *impl = "aos";
#else
    const char *impl = "soa";
#endif

    // Warmup
    {
        thread_t t;
        thread_create(&t, worker_a, NULL);
        thread_join(t, NULL);
    }

    // Dup hit
    fprintf(stderr, "%s,dup_hit,100000,%.1f\n", impl, bench_dup_hit(100000));

    // Cycle
    fprintf(stderr, "%s,cycle,10000,%.1f\n", impl, bench_cycle(10000));

    // Miss sous charge
    int sizes[] = {10, 100, 1000, 3000};
    for (int i = 0; i < 4; i++)
        fprintf(stderr, "%s,miss,%d,%.1f\n", impl, sizes[i], bench_miss_loaded(sizes[i], 200));

    return 0;
}
