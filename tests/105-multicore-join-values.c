#include "thread.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

/* Test de la valeur de retour via thread_join.
 *
 * Crée N threads, chacun retourne une valeur unique calculée à partir
 * de son index. On vérifie que thread_join renvoie exactement la bonne
 * valeur pour chaque thread.
 *
 * Le test échoue si une valeur ne correspond pas.
 */

static void *thfunc(void *arg)
{
    unsigned long i = (unsigned long)arg;
    unsigned long val = i * 0x1234567890ABCDEFULL + 0xDEAD;
    return (void *)val;
}

int main(int argc, char *argv[])
{
    int n = 200;
    if (argc > 1)
        n = atoi(argv[1]);

    thread_t *threads = malloc(n * sizeof(thread_t));
    assert(threads != NULL);

    int err;
    for (int i = 0; i < n; i++) {
        err = thread_create(&threads[i], thfunc, (void *)(unsigned long)i);
        assert(!err);
    }

    int mismatches = 0;
    for (int i = 0; i < n; i++) {
        void *res = NULL;
        err = thread_join(threads[i], &res);
        assert(!err);

        unsigned long expected = (unsigned long)i * 0x1234567890ABCDEFULL + 0xDEAD;
        if ((unsigned long)res != expected) {
            fprintf(stderr, "MISMATCH thread %d: got 0x%lx, expected 0x%lx\n",
                    i, (unsigned long)res, expected);
            mismatches++;
        }
    }

    free(threads);

    if (mismatches > 0) {
        printf("FAIL: %d/%d join value mismatches\n", mismatches, n);
        return EXIT_FAILURE;
    }

    printf("join values OK: %d threads, all return values correct\n", n);
    return EXIT_SUCCESS;
}
