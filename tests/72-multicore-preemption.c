/* Test de validation de la préemption en mode multi-cœur.
 *
 * Idée : on lance N threads compute-bound (jamais de yield, ni mutex, ni
 * sync) avec N strictement supérieur au nombre de workers. Sans préemption,
 * exactement `nworkers` threads (les premiers élus) monopolisent leur
 * worker à vie, et les autres ne progressent JAMAIS — leur compteur reste
 * à zéro.
 *
 * Avec préemption SIGALRM active, le scheduler force régulièrement des
 * yields, ce qui permet aux threads "starved" d'être ré-élus. À la fin du
 * test, TOUS les compteurs doivent avoir progressé (> 0). Cela démontre
 * en plus :
 *   - que le multi-cœur fonctionne (sinon un seul thread tournerait à la
 *     fois → throughput = capacité 1 cœur),
 *   - que la préemption fonctionne en parallèle sur tous les workers
 *     (sinon les workers != 0 ne préempteraient jamais leurs threads).
 *
 * Le test échoue si :
 *   - un seul thread n'a pas progressé (preuve qu'il n'a jamais été élu),
 *   - le throughput total reste sous ~1.5x la capacité d'un seul thread
 *     (preuve qu'il n'y a pas de vrai parallélisme).
 *
 * Usage : ./72-multicore-preemption [N]
 *   N = nombre de threads créés (default : 4 × nworkers détecté).
 *
 * Compile/run en MN+preempt :
 *   make MULTICORE=1 EXTRA_CFLAGS="-DLIBTHREAD_MN_PREEMPT" tests/72-multicore-preemption
 *   LD_LIBRARY_PATH=. ./tests/72-multicore-preemption
 */

#include "thread.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <unistd.h>

#define DURATION_US 300000  /* 300 ms */

struct padded_counter {
    volatile long count;
    char pad[64 - sizeof(long)];
};

static volatile int stop_flag = 0;
static struct padded_counter *counters = NULL;

static long now_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000000L + tv.tv_usec;
}

/* Boucle compute-bound : aucun yield, aucun appel système.
 * Sans préemption, ce thread monopolise son worker jusqu'à stop_flag. */
__attribute__((used)) static void *busy(void *arg)
{
    int id = (intptr_t)arg;
    while (!stop_flag) {
        counters[id].count++;
    }
    return NULL;
}

/* Lance `n` threads `busy`, attend `duration_us` (le main yield activement),
 * arrête tout, join, et retourne le total des compteurs. */
static long run_busy_phase(int n, long duration_us)
{
    thread_t *th = malloc(n * sizeof(*th));
    if (!th) { perror("malloc"); exit(EXIT_FAILURE); }
    for (int i = 0; i < n; i++) counters[i].count = 0;

    stop_flag = 0;
    for (int i = 0; i < n; i++) {
        int err = thread_create(&th[i], busy, (void *)(intptr_t)i);
        if (err != 0) { fprintf(stderr, "thread_create failed\n"); exit(EXIT_FAILURE); }
    }

    long t_start = now_us();
    while (now_us() - t_start < duration_us) {
        thread_yield();
    }
    stop_flag = 1;

    for (int i = 0; i < n; i++) {
        int err = thread_join(th[i], NULL);
        if (err != 0) { fprintf(stderr, "thread_join failed\n"); exit(EXIT_FAILURE); }
    }
    free(th);

    long total = 0;
    for (int i = 0; i < n; i++) total += counters[i].count;
    return total;
}

int main(int argc, char *argv[])
{
    long nworkers_env = 0;
    const char *env = getenv("LIBTHREAD_WORKERS");
    if (env) nworkers_env = atol(env);
    if (nworkers_env <= 0)
        nworkers_env = sysconf(_SC_NPROCESSORS_ONLN);
    if (nworkers_env <= 0) nworkers_env = 4;

    int nb = (argc >= 2) ? atoi(argv[1]) : (int)(4 * nworkers_env);
    if (nb < 2) {
        fprintf(stderr, "nombre de threads invalide (%d)\n", nb);
        return EXIT_FAILURE;
    }

    printf("Workers détectés: %ld, threads créés: %d\n", nworkers_env, nb);

    /* Le tableau de compteurs est dimensionné au max (nb threads). */
    counters = calloc(nb, sizeof(*counters));
    if (!counters) { perror("calloc"); return EXIT_FAILURE; }

    /* Phase 1 — Calibrer la capacité d'1 thread compute solo.
     * Un seul thread, mêmes opérations que la phase 2 → comparaison juste.
     * On laisse seulement (nworkers-1) workers idle pour ce test, le main
     * yield et laisse le thread compute monopoliser un worker. */
    long single_total = run_busy_phase(1, DURATION_US);
    printf("Baseline 1 thread solo : %ld counts en %d ms\n",
           single_total, DURATION_US / 1000);

    /* Phase 2 — N threads compute en parallèle. */
    long t_start = now_us();
    long total = run_busy_phase(nb, DURATION_US);
    long real_duration = now_us() - t_start;

    int progressed = 0;
    long min_count = counters[0].count, max_count = counters[0].count;
    for (int i = 0; i < nb; i++) {
        long c = counters[i].count;
        if (c > 0) progressed++;
        if (c < min_count) min_count = c;
        if (c > max_count) max_count = c;
    }
    int idle_threads = nb - progressed;
    double parallel_factor = (double)total / (double)single_total;

    printf("Multi-thread : %d threads, %ld total counts en ~%ld ms\n",
           nb, total, real_duration / 1000);
    printf("Threads ayant progressé : %d/%d (idle: %d)\n", progressed, nb, idle_threads);
    printf("Min: %ld, Max: %ld, ratio min/max: %.3f\n", min_count, max_count,
           max_count > 0 ? (double)min_count / (double)max_count : 0.0);
    printf("Parallélisme : %.2fx un thread solo\n", parallel_factor);

    free(counters);

    /* Critère 1 — préemption : aucun thread ne doit être resté à zéro.
     * Sans préemption, exactement (nb - nworkers) threads seraient idle. */
    if (idle_threads > 0) {
        printf("ÉCHEC: %d threads n'ont jamais été élus — la préemption "
               "ne fonctionne pas (ou pas sur tous les workers).\n", idle_threads);
        return EXIT_FAILURE;
    }

    /* Critère 2 — multi-cœur : throughput agrégé > 1.5× un thread solo.
     * Sur un système avec >= 2 workers réels, on s'attend à 1.5-Nx. */
    if (nworkers_env >= 2 && parallel_factor < 1.5) {
        printf("ÉCHEC: parallélisme %.2fx insuffisant pour %ld workers.\n",
               parallel_factor, nworkers_env);
        return EXIT_FAILURE;
    }

    printf("OK: préemption + multi-cœur fonctionnent (%d threads tous "
           "progressent, %.2fx parallélisme).\n", nb, parallel_factor);
    return EXIT_SUCCESS;
}
