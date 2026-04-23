/* Producteur/consommateur avec buffer borné.
 * Utilise 3 sémaphores : mutex + empty + full.
 * Vérifie qu'aucun élément n'est perdu ni dupliqué. */
#include "thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <stdint.h>

#define BUF_SIZE   8
#define N_ITEMS    64
#define N_PROD     4
#define N_CONS     4

static int      buffer[BUF_SIZE];
static int      buf_in  = 0;
static int      buf_out = 0;
static thread_sem_t mutex_sem;   /* accès exclusif au buffer */
static thread_sem_t empty_sem;   /* nb de slots vides        */
static thread_sem_t full_sem;    /* nb de slots pleins       */

static int produced[N_ITEMS];    /* produit[i] = 1 si item i produit */
static int consumed[N_ITEMS];    /* consumed[i] = 1 si item i consommé */

static void *producer(void *arg)
{
    int base = (int)(intptr_t)arg * (N_ITEMS / N_PROD);
    for (int i = 0; i < N_ITEMS / N_PROD; i++) {
        int item = base + i;
        thread_sem_wait(&empty_sem);
        thread_sem_wait(&mutex_sem);
        buffer[buf_in] = item;
        buf_in = (buf_in + 1) % BUF_SIZE;
        produced[item] = 1;
        thread_sem_post(&mutex_sem);
        thread_sem_post(&full_sem);
    }
    return NULL;
}

static void *consumer(void *arg)
{
    (void)arg;
    for (int i = 0; i < N_ITEMS / N_CONS; i++) {
        thread_sem_wait(&full_sem);
        thread_sem_wait(&mutex_sem);
        int item = buffer[buf_out];
        buf_out = (buf_out + 1) % BUF_SIZE;
        consumed[item] = 1;
        thread_sem_post(&mutex_sem);
        thread_sem_post(&empty_sem);
    }
    return NULL;
}

int main(void)
{
    thread_sem_init(&mutex_sem, 1);
    thread_sem_init(&empty_sem, BUF_SIZE);
    thread_sem_init(&full_sem,  0);

    thread_t prod[N_PROD], cons[N_CONS];
    for (int i = 0; i < N_PROD; i++)
        thread_create(&prod[i], producer, (void *)(intptr_t)i);
    for (int i = 0; i < N_CONS; i++)
        thread_create(&cons[i], consumer, NULL);

    for (int i = 0; i < N_PROD; i++) thread_join(prod[i], NULL);
    for (int i = 0; i < N_CONS; i++) thread_join(cons[i], NULL);

    /* Vérification : chaque item produit exactement une fois et consommé une fois */
    int ok = 1;
    for (int i = 0; i < N_ITEMS; i++) {
        if (!produced[i])  { printf("ERREUR: item %d non produit\n",  i); ok = 0; }
        if (!consumed[i])  { printf("ERREUR: item %d non consommé\n", i); ok = 0; }
    }
    if (ok) printf("Producteur/consommateur OK (%d items)\n", N_ITEMS);

    thread_sem_destroy(&mutex_sem);
    thread_sem_destroy(&empty_sem);
    thread_sem_destroy(&full_sem);
    return ok ? 0 : 1;
}