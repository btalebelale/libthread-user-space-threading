/* Test multicore : producteur/consommateur avec semaphores.
 * 4 producteurs, 4 consommateurs, buffer circulaire de 16 slots.
 * 10000 items au total. Verification via bitmap atomique.
 *
 * support necessaire:
 * - thread_create()
 * - thread_join()
 * - thread_sem_init/destroy/wait/post()
 */
#include "thread.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/time.h>

#define BUF_SIZE  16
#define N_ITEMS   10000
#define N_PROD    4
#define N_CONS    4

static int buffer[BUF_SIZE];
static int buf_in  = 0;
static int buf_out = 0;

static thread_sem_t mutex_sem;
static thread_sem_t empty_sem;
static thread_sem_t full_sem;

static int produced_count = 0;
static int produced_bitmap[N_ITEMS];
static int consumed_bitmap[N_ITEMS];

static void *producer(void *arg)
{
    (void)arg;
    for (;;) {
        int item = __atomic_fetch_add(&produced_count, 1, __ATOMIC_SEQ_CST);
        if (item >= N_ITEMS)
            break;

        thread_sem_wait(&empty_sem);
        thread_sem_wait(&mutex_sem);

        buffer[buf_in] = item;
        buf_in = (buf_in + 1) % BUF_SIZE;
        __atomic_store_n(&produced_bitmap[item], 1, __ATOMIC_SEQ_CST);

        thread_sem_post(&mutex_sem);
        thread_sem_post(&full_sem);
    }
    return NULL;
}

static void *consumer(void *arg)
{
    int count = (int)(intptr_t)arg;
    for (int i = 0; i < count; i++) {
        thread_sem_wait(&full_sem);
        thread_sem_wait(&mutex_sem);

        int item = buffer[buf_out];
        buf_out = (buf_out + 1) % BUF_SIZE;
        __atomic_store_n(&consumed_bitmap[item], 1, __ATOMIC_SEQ_CST);

        thread_sem_post(&mutex_sem);
        thread_sem_post(&empty_sem);
    }
    return NULL;
}

int main(void)
{
    memset(produced_bitmap, 0, sizeof(produced_bitmap));
    memset(consumed_bitmap, 0, sizeof(consumed_bitmap));

    thread_sem_init(&mutex_sem, 1);
    thread_sem_init(&empty_sem, BUF_SIZE);
    thread_sem_init(&full_sem, 0);

    thread_t prod[N_PROD], cons[N_CONS];
    int err;

    struct timeval tv1, tv2;
    gettimeofday(&tv1, NULL);

    for (int i = 0; i < N_PROD; i++) {
        err = thread_create(&prod[i], producer, NULL);
        assert(!err);
    }
    for (int i = 0; i < N_CONS; i++) {
        err = thread_create(&cons[i], consumer,
                            (void *)(intptr_t)(N_ITEMS / N_CONS));
        assert(!err);
    }

    for (int i = 0; i < N_PROD; i++) {
        err = thread_join(prod[i], NULL);
        assert(!err);
    }
    for (int i = 0; i < N_CONS; i++) {
        err = thread_join(cons[i], NULL);
        assert(!err);
    }

    gettimeofday(&tv2, NULL);

    thread_sem_destroy(&mutex_sem);
    thread_sem_destroy(&empty_sem);
    thread_sem_destroy(&full_sem);

    unsigned long us = (tv2.tv_sec - tv1.tv_sec) * 1000000
                     + (tv2.tv_usec - tv1.tv_usec);

    int ok = 1;
    for (int i = 0; i < N_ITEMS; i++) {
        if (!produced_bitmap[i]) {
            printf("ERREUR: item %d non produit\n", i);
            ok = 0;
        }
        if (!consumed_bitmap[i]) {
            printf("ERREUR: item %d non consomme\n", i);
            ok = 0;
        }
    }

    if (ok) {
        printf("Producteur/consommateur multicore OK: %d items, "
               "%d prod, %d cons en %lu us\n",
               N_ITEMS, N_PROD, N_CONS, us);
        printf("GRAPH;103;%d;%lu\n", N_ITEMS, us);
        return EXIT_SUCCESS;
    } else {
        printf("ERREUR: items perdus ou dupliques\n");
        return EXIT_FAILURE;
    }
}
