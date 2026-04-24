#include "thread.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

/* Récursion infinie → déborde la pile.
 * noinline + noclone empêchent le compilateur de transformer en tail-call */
__attribute__((noinline, noclone))
static void *overflow_func(void *dummy)
{
    volatile char buf[4096];
    buf[0] = 1;
    buf[4095] = 2;
    /* Utiliser le buffer pour empêcher son élimination */
    if (buf[0] + buf[4095] == 0) return dummy;
    void *(*volatile self)(void *) = overflow_func;
    void *ret = self(dummy);
    /* Empêcher la tail-call : utiliser ret après l'appel */
    *(volatile char *)&buf[1] = (char)(long)ret;
    return ret;
}

static void *good_func(void *arg)
{
    return arg;
}

int main(void)
{
    thread_t t_bad, t_good;
    void *res;
    int err;

    /* Thread normal - créer et joiner tout de suite */
    err = thread_create(&t_good, good_func, (void *)42);
    assert(!err);
    err = thread_join(t_good, &res);
    printf("join good = %d, res = %ld\n", err, (long)res);
    assert(err == 0);
    assert(res == (void *)42);

    /* Thread qui va déborder */
    err = thread_create(&t_bad, overflow_func, NULL);
    assert(!err);

    /* Yield pour forcer le flush (allocation de sa propre stack) */
    thread_yield();

    err = thread_join(t_bad, &res);
    printf("join overflow = %d (attendu: non-zero)\n", err);
    assert(err != 0);

    /* Thread normal après le crash - vérifie que le système survit */
    err = thread_create(&t_good, good_func, (void *)84);
    assert(!err);
    err = thread_join(t_good, &res);
    printf("join good2 = %d, res = %ld\n", err, (long)res);
    assert(err == 0);
    assert(res == (void *)84);

    printf("OK: stack overflow detecte, autres threads non affectes\n");
    return EXIT_SUCCESS;
}
