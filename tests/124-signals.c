#include "thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

static thread_t sender_id;
static thread_t waiter_id;

void *waiter(void *arg)
{
    (void)arg;
    int sig = -1;
    printf("waiter: appel thread_sigwait, masque = 0b00000110 (sigs 1 et 2)\n");
    assert(thread_sigwait(0b00000110, &sig) == 0);
    printf("waiter: réveillé par signal %d\n", sig);
    assert(sig == 2);
    return NULL;
}

void *sender(void *arg)
{
    (void)arg;
    printf("sender: envoi signal 2 au waiter\n");
    thread_kill(waiter_id, 2);
    printf("sender: signal envoyé\n");
    return NULL;
}

int main(void)
{
    thread_create(&waiter_id, waiter, NULL);
    thread_create(&sender_id, sender, NULL);
    thread_join(waiter_id, NULL);
    thread_join(sender_id, NULL);
    printf("OK\n");
    return 0;
}