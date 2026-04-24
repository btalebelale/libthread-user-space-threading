#include "thread.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

/* Test de l'exclusion mutuelle.
 *
 * N threads font chacun 10000 cycles lock/unlock sur un mutex unique.
 * Dans la section critique, on vérifie atomiquement qu'un seul thread
 * est présent. Toute violation est comptabilisée.
 *
 * Le test échoue si violations > 0.
 */

#define ITERATIONS 10000

static thread_mutex_t mutex;
static volatile int inside = 0;
static volatile int violations = 0;

static void *thfunc(void *dummy __attribute__((unused)))
{
    for (int i = 0; i < ITERATIONS; i++) {
        thread_mutex_lock(&mutex);

        int val = __atomic_fetch_add(&inside, 1, __ATOMIC_SEQ_CST);
        if (val + 1 > 1) {
            __atomic_fetch_add((volatile int *)&violations, 1, __ATOMIC_SEQ_CST);
        }

        /* Small amount of work inside critical section */
        volatile int dummy_work = 0;
        for (int j = 0; j < 10; j++)
            dummy_work++;
        (void)dummy_work;

        __atomic_fetch_sub(&inside, 1, __ATOMIC_SEQ_CST);

        thread_mutex_unlock(&mutex);
    }
    return NULL;
}

int main(int argc, char *argv[])
{
    int n = 8;
    if (argc > 1)
        n = atoi(argv[1]);

    thread_mutex_init(&mutex);

    thread_t *threads = malloc(n * sizeof(thread_t));
    assert(threads != NULL);

    int err;
    for (int i = 0; i < n; i++) {
        err = thread_create(&threads[i], thfunc, NULL);
        assert(!err);
    }

    for (int i = 0; i < n; i++) {
        err = thread_join(threads[i], NULL);
        assert(!err);
    }

    thread_mutex_destroy(&mutex);
    free(threads);

    if (violations > 0) {
        printf("FAIL: %d mutual exclusion violations detected\n", violations);
        return EXIT_FAILURE;
    }

    printf("mutex ordering OK: %d threads x %d iterations, 0 violations\n",
           n, ITERATIONS);
    return EXIT_SUCCESS;
}
