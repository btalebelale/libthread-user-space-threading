/* Test multicore : cout du mutex sous contention.
 * Pour T dans {1, 2, 4, 8} : T threads font 1000000/T lock/unlock
 * chacun sur un meme mutex. Mesure ns/op.
 *
 * support necessaire:
 * - thread_create()
 * - thread_join()
 * - thread_mutex_init/lock/unlock/destroy()
 */
#include "thread.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define TOTAL_OPS 1000000

static thread_mutex_t mutex;
static volatile int counter;

struct thread_arg {
    int ops;
};

static void *thfunc(void *arg)
{
    struct thread_arg *ta = (struct thread_arg *)arg;
    for (int i = 0; i < ta->ops; i++) {
        thread_mutex_lock(&mutex);
        counter++;
        thread_mutex_unlock(&mutex);
    }
    return NULL;
}

static long timespec_ns(struct timespec *start, struct timespec *end)
{
    return (end->tv_sec - start->tv_sec) * 1000000000L
         + (end->tv_nsec - start->tv_nsec);
}

int main(void)
{
    int thread_counts[] = {1, 2, 4, 8};
    int n_configs = 4;

    printf("%-10s %10s\n", "Threads", "ns/op");
    printf("--------- ----------\n");

    for (int c = 0; c < n_configs; c++) {
        int nt = thread_counts[c];
        int ops_per_thread = TOTAL_OPS / nt;

        thread_mutex_init(&mutex);
        counter = 0;

        thread_t *th = malloc(nt * sizeof(*th));
        struct thread_arg *args = malloc(nt * sizeof(*args));
        if (!th || !args) {
            perror("malloc");
            return EXIT_FAILURE;
        }

        struct timespec t1, t2;
        int err;

        clock_gettime(CLOCK_MONOTONIC, &t1);
        for (int i = 0; i < nt; i++) {
            args[i].ops = ops_per_thread;
            err = thread_create(&th[i], thfunc, &args[i]);
            assert(!err);
        }
        for (int i = 0; i < nt; i++) {
            err = thread_join(th[i], NULL);
            assert(!err);
        }
        clock_gettime(CLOCK_MONOTONIC, &t2);

        thread_mutex_destroy(&mutex);

        long ns = timespec_ns(&t1, &t2);
        long ns_per_op = ns / (nt * ops_per_thread);

        printf("T=%d       %10ld\n", nt, ns_per_op);
        printf("GRAPH;122;%d;%ld\n", nt, ns_per_op);

        free(th);
        free(args);
    }

    return EXIT_SUCCESS;
}
