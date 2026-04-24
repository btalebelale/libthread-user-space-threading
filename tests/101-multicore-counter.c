/* Test multicore : N threads incrementent un compteur partage sous mutex.
 * Pas de thread_yield() dans la boucle — stress pur sur le mutex.
 * Verifie counter == N * ITERS.
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
#include <sys/time.h>

#define ITERS 100000

static int counter = 0;
static thread_mutex_t lock;

static void *thfunc(void *dummy __attribute__((unused)))
{
    for (int i = 0; i < ITERS; i++) {
        thread_mutex_lock(&lock);
        counter++;
        thread_mutex_unlock(&lock);
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
    if (!th) {
        perror("malloc");
        return EXIT_FAILURE;
    }

    int err = thread_mutex_init(&lock);
    assert(!err);

    struct timeval tv1, tv2;
    gettimeofday(&tv1, NULL);

    for (int i = 0; i < nb; i++) {
        err = thread_create(&th[i], thfunc, NULL);
        assert(!err);
    }

    for (int i = 0; i < nb; i++) {
        err = thread_join(th[i], NULL);
        assert(!err);
    }

    gettimeofday(&tv2, NULL);

    thread_mutex_destroy(&lock);
    free(th);

    unsigned long us = (tv2.tv_sec - tv1.tv_sec) * 1000000
                     + (tv2.tv_usec - tv1.tv_usec);

    int expected = nb * ITERS;
    if (counter == expected) {
        printf("Compteur correct: %d threads * %d iters = %d en %lu us\n",
               nb, ITERS, counter, us);
        printf("GRAPH;101;%d;%lu\n", nb, us);
        return EXIT_SUCCESS;
    } else {
        printf("ERREUR: attendu %d, obtenu %d\n", expected, counter);
        return EXIT_FAILURE;
    }
}
