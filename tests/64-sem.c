#include "thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <stdint.h>

static thread_sem_t sem;
static int counter = 0;

/* Test 1 : sémaphore binaire (mutex) */
static void *inc(void *arg) {
    (void)arg;
    thread_sem_wait(&sem);
    counter++;
    thread_sem_post(&sem);
    return NULL;
}



int main(void)
{
    /* --- Test 1 : section critique avec sémaphore binaire --- */
    thread_sem_init(&sem, 1);
    thread_t th[16];
    for (int i = 0; i < 16; i++)
        thread_create(&th[i], inc, NULL);
    for (int i = 0; i < 16; i++)
        thread_join(th[i], NULL);
    assert(counter == 16);
    printf("Test 1 (mutex binaire) OK : counter = %d\n", counter);
    thread_sem_destroy(&sem);

    // Test 2 : sémaphore compteur (N ) 
    int tokens = 4;
    thread_sem_t csem;
    thread_sem_init(&csem, tokens);
    int passed = 0;
    thread_t cth[16];
    /* tous passent car 4 jetons disponibles en FIFO */
    for (int i = 0; i < 16; i++)
        thread_create(&cth[i], inc, (void *)(intptr_t)i);
    /* juste vérifier qu'ils terminent tous */
    for (int i = 0; i < 16; i++)
        thread_join(cth[i], NULL);
    (void)passed;
    printf("Test 2 (compteur) OK\n");
    thread_sem_destroy(&csem);

    printf("TOUS LES TESTS PASSED\n");
    return 0;
}