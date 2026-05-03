/* Multi-core (M:N) variant of thread.c.
 * Build with -DMULTICORE; pure-mono build still uses src/thread.c. */

#define _GNU_SOURCE
#include <sched.h>
#include "thread.h"
#include "thread_internal.h"
#include "scheduler_mn.h"
#include "thread_common.h"
#include "preempt.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <setjmp.h>

#define MAX_WORKERS 128

__attribute__((visibility("hidden"))) thread_hot_t  *hot_slab;
__attribute__((visibility("hidden"))) thread_cold_t *cold_slab;
static int            thread_slab_next;
static thread_hot_t  *thread_free_head;

static void *stack_arena;
static int   stack_arena_next;
static void *stack_free_head;

static pthread_spinlock_t alloc_lock;
#ifndef LIBTHREAD_NO_DEADLOCK_DETECT
static pthread_spinlock_t uf_lock;
#endif

static int      nworkers;
static worker_t workers[MAX_WORKERS] __attribute__((aligned(64)));
static int system_ready;
static int alive_count;  /* accédé via __atomic_* */
static int system_destroyed;

/* Symbols referenced by context_switch.S — never accessed from C in MN mode. */
__attribute__((visibility("hidden"))) thread_hot_t *current __attribute__((aligned(64)));
__attribute__((visibility("hidden"))) thread_hot_t *last_created;
__attribute__((visibility("hidden"))) int inline_executing;

/* ---------- Preemption ----------
 * En mode M:N, la préemption SIGALRM est DÉSACTIVÉE par défaut : un signal
 * tombant sur la sched_stack peut corrompre la séquence schedule/context_switch.
 * USE_PREEMPTION peut rester défini (il l'est dans CFLAGS pour bloquer l'alias
 * du .S), mais le code SIGALRM n'est pas compilé. Pour forcer la préemption en
 * MN (e.g. test 71), définir LIBTHREAD_MN_PREEMPT à la compilation.
 * Helpers block/restore/unblock_self : voir preempt.h. */
#ifdef PREEMPT_ENABLED
#define PREEMPT_INTERVAL_US 200

static void preempt_handler(int sig)
{
    (void)sig;
    preempt_unblock_self();
    if (self_worker && self_worker->current && self_worker->inline_executing == 0)
        thread_yield();
}

__attribute__((cold)) static void preempt_init(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = preempt_handler;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGALRM, &sa, NULL);

    struct itimerval timer;
    timer.it_value.tv_sec = 0;
    timer.it_value.tv_usec = PREEMPT_INTERVAL_US;
    timer.it_interval = timer.it_value;
    setitimer(ITIMER_REAL, &timer, NULL);
}
#define MN_HAS_PREEMPT 1
#else
#define preempt_init() ((void)0)
#endif

/* ---------- Allocateurs (verrouillés global) ---------- */
__attribute__((cold)) static void mem_init(void)
{
    stack_arena = mmap(NULL, (size_t)STACK_CAP * STACK_SIZE, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stack_arena == MAP_FAILED) { perror("mmap stack arena"); exit(1); }
    madvise(stack_arena, (size_t)STACK_CAP * STACK_SIZE, MADV_HUGEPAGE);

    hot_slab = mmap(NULL, (size_t)THREAD_CAP * sizeof(thread_hot_t), PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (hot_slab == MAP_FAILED) { perror("mmap hot slab"); exit(1); }

    cold_slab = mmap(NULL, (size_t)THREAD_CAP * sizeof(thread_cold_t), PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (cold_slab == MAP_FAILED) { perror("mmap cold slab"); exit(1); }

    stack_arena_next = 0;
    stack_free_head  = NULL;
    thread_slab_next = 0;
    thread_free_head = NULL;

    pthread_spin_init(&alloc_lock, PTHREAD_PROCESS_PRIVATE);
#ifndef LIBTHREAD_NO_DEADLOCK_DETECT
    pthread_spin_init(&uf_lock, PTHREAD_PROCESS_PRIVATE);
#endif
}

__attribute__((cold)) static void mem_destroy(void)
{
    if (stack_arena && stack_arena != MAP_FAILED)
        munmap(stack_arena, (size_t)STACK_CAP * STACK_SIZE);
    if (hot_slab && hot_slab != MAP_FAILED)
        munmap(hot_slab, (size_t)THREAD_CAP * sizeof(thread_hot_t));
    if (cold_slab && cold_slab != MAP_FAILED)
        munmap(cold_slab, (size_t)THREAD_CAP * sizeof(thread_cold_t));
    stack_arena = NULL;
    hot_slab    = NULL;
    cold_slab   = NULL;
}

static void *stack_alloc_locked(void)
{
    if (stack_free_head != NULL) {
        void *s = stack_free_head;
        stack_free_head = *STACK_FREELIST_PTR(s);
        return s;
    }
    if (stack_arena_next >= STACK_CAP) return NULL;
    void *s = (char *)stack_arena + ((size_t)stack_arena_next++ * STACK_SIZE);
    /* Première utilisation de ce slot : poser la page de garde en bas.
     * La protection persiste à travers les recyclages via free list. */
    if (mprotect(s, STACK_GUARD, PROT_NONE) != 0)
        perror("mprotect stack guard");
    return s;
}

/* ---------- Caches TLS pour thread descriptors et stacks ----------
 * Chaque pthread garde une free-list locale, refill par batch depuis le
 * pool global. Élimine alloc_lock du hot path tant que les ops alloc/release
 * sont équilibrées par pthread. */
#define T_CACHE_MAX     32
#define T_CACHE_REFILL  16
#define S_CACHE_MAX     16
#define S_CACHE_REFILL  8

static __thread thread_hot_t *t_cache_head;
static __thread int           t_cache_count;
static __thread void          *s_cache_head;
static __thread int           s_cache_count;

static void *stack_alloc(void)
{
    if (s_cache_head) {
        void *s = s_cache_head;
        s_cache_head = *STACK_FREELIST_PTR(s);
        s_cache_count--;
        return s;
    }
    /* Refill : prend jusqu'à S_CACHE_REFILL stacks dans le pool global. */
    pthread_spin_lock(&alloc_lock);
    void *first = NULL;
    int got = 0;
    while (got < S_CACHE_REFILL) {
        void *s = stack_alloc_locked();
        if (!s) break;
        *STACK_FREELIST_PTR(s) = first;
        first = s;
        got++;
    }
    pthread_spin_unlock(&alloc_lock);
    if (!first) return NULL;
    /* first reste pour le caller, le reste va dans le cache. */
    void *ret = first;
    void *rest = *STACK_FREELIST_PTR(first);
    while (rest) {
        void *next = *STACK_FREELIST_PTR(rest);
        *STACK_FREELIST_PTR(rest) = s_cache_head;
        s_cache_head = rest;
        s_cache_count++;
        rest = next;
    }
    return ret;
}

static void stack_release(void *s)
{
    if (s_cache_count < S_CACHE_MAX) {
        *STACK_FREELIST_PTR(s) = s_cache_head;
        s_cache_head = s;
        s_cache_count++;
        return;
    }
    /* Cache plein : flush une moitié vers global, puis on ajoute s. */
    pthread_spin_lock(&alloc_lock);
    for (int i = 0; i < S_CACHE_REFILL && s_cache_head; i++) {
        void *x = s_cache_head;
        s_cache_head = *STACK_FREELIST_PTR(x);
        s_cache_count--;
        *STACK_FREELIST_PTR(x) = stack_free_head;
        stack_free_head = x;
    }
    *STACK_FREELIST_PTR(s) = stack_free_head;
    stack_free_head = s;
    pthread_spin_unlock(&alloc_lock);
}

static thread_hot_t *thread_alloc(void)
{
    if (t_cache_head) {
        thread_hot_t *t = t_cache_head;
        t_cache_head = (thread_hot_t *)t->rsp;
        t_cache_count--;
        return t;
    }
    /* Refill : free-list global d'abord, puis slab vierge.
     * On chaîne via t->rsp comme le pool global le fait déjà. */
    pthread_spin_lock(&alloc_lock);
    thread_hot_t *list = NULL;
    int got = 0;
    while (got < T_CACHE_REFILL && thread_free_head) {
        thread_hot_t *t = thread_free_head;
        thread_free_head = (thread_hot_t *)t->rsp;
        t->rsp = (void *)list;
        list = t;
        got++;
    }
    while (got < T_CACHE_REFILL && thread_slab_next < THREAD_CAP) {
        thread_hot_t *t = &hot_slab[thread_slab_next++];
        thread_cold_t *tc = THREAD_COLD(t);
        pthread_spin_init(&tc->cold_lock, PTHREAD_PROCESS_PRIVATE);
        t->rsp = (void *)list;
        list = t;
        got++;
    }
    pthread_spin_unlock(&alloc_lock);
    if (!list) return NULL;
    thread_hot_t *ret = list;
    list = (thread_hot_t *)list->rsp;
    ret->rsp = NULL;
    while (list) {
        thread_hot_t *next = (thread_hot_t *)list->rsp;
        list->rsp = (void *)t_cache_head;
        t_cache_head = list;
        t_cache_count++;
        list = next;
    }
    return ret;
}

static void thread_release(thread_hot_t *t)
{
    if (t_cache_count < T_CACHE_MAX) {
        t->rsp = (void *)t_cache_head;
        t_cache_head = t;
        t_cache_count++;
        return;
    }
    /* Cache plein : flush moitié vers global, puis on ajoute t. */
    pthread_spin_lock(&alloc_lock);
    for (int i = 0; i < T_CACHE_REFILL && t_cache_head; i++) {
        thread_hot_t *x = t_cache_head;
        t_cache_head = (thread_hot_t *)x->rsp;
        t_cache_count--;
        x->rsp = (void *)thread_free_head;
        thread_free_head = x;
    }
    t->rsp = (void *)thread_free_head;
    thread_free_head = t;
    pthread_spin_unlock(&alloc_lock);
}

/* ---------- Stack init / trampoline ---------- */
static int stack_color_counter;

static void *stack_init(void *stack_base, void *(*func)(void *), void *arg)
{
    uintptr_t sp = (uintptr_t)stack_base + STACK_SIZE;
    sp -= ((unsigned)__atomic_fetch_add(&stack_color_counter, 1, __ATOMIC_RELAXED) & 0xFF) * 64;
    sp &= ~(uintptr_t)0xF;

    void **s = (void **)sp;
    *(--s) = (void *)thread_trampoline;
    *(--s) = 0;            /* rbx */
    *(--s) = 0;            /* rbp */
    *(--s) = (void *)func; /* r12 */
    *(--s) = arg;          /* r13 */
    *(--s) = 0;            /* r14 */
    *(--s) = 0;            /* r15 */
    VALGRIND_MAKE_MEM_DEFINED(s, 7 * sizeof(void *));
    return s;
}

__attribute__((noinline, cold, visibility("hidden")))
void lazy_stack_alloc(thread_hot_t *t)
{
    thread_cold_t *tc = THREAD_COLD(t);
    void *stack = stack_alloc();
    if (!stack) {
        /* L'arena de stacks est épuisée. Pas de fallback : exécuter inline
         * sur la sched_stack ne marche pas (corruption + race sur tc->state).
         * On abort proprement, c'est une erreur de capacité non récupérable. */
        fprintf(stderr,
                "libthread: stack arena exhausted (max %d threads concurrents)\n",
                STACK_CAP);
        abort();
    }
    tc->stack_base = stack;
    t->rsp = stack_init(stack, tc->func, tc->func_arg);
#ifndef NVALGRIND
    /* Stack utilisable = [stack + GUARD, stack + STACK_SIZE). */
    tc->valgrind_stackid = VALGRIND_STACK_REGISTER(
        (char *)stack + STACK_GUARD, (char *)stack + STACK_SIZE);
#endif
}

/* Symbole de stub pour le linker (référencé dans context_switch.S, jamais appelé en MN). */
__attribute__((noinline, cold, visibility("hidden")))
void flush_last_created_slow(void)
{
}

/* Flush du last_created TLS vers la runqueue. */
static inline void flush_last_created(void)
{
    worker_t *w = self_worker;
    if (__builtin_expect(w->last_created != NULL, 0)) {
        thread_hot_t *t = w->last_created;
        thread_cold_t *tc = THREAD_COLD(t);
        tc->func     = w->last_created_func;
        tc->func_arg = w->last_created_arg;
        w->last_created = NULL;
        runq_push(t);
    }
}

/* ---------- CPU affinity ----------
 * Pin worker[i] sur CPU (i % NCPU). Stabilise les benchs (pas de migration
 * kernel) et garde les caches L1/L2 chauds par worker. Best-effort : si la
 * call échoue, on continue sans pinning (utile sur hôtes contraints). */
__attribute__((cold)) static void pin_worker_to_cpu(int cpu)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

/* ---------- Worker entry (pour les pthreads non-main) ---------- */
__attribute__((__noreturn__)) static void *worker_entry(void *arg)
{
    worker_t *w = (worker_t *)arg;
    self_worker = w;
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu > 0) pin_worker_to_cpu(w->id % (int)ncpu);
#ifdef MN_HAS_PREEMPT
    preempt_unblock_self();
#endif
    /* La sched_stack est la pile pthread elle-même : sched_rsp sera sauvé
     * automatiquement au premier context_switch dans worker_loop. */
    worker_loop();
}

/* ---------- thread_entry : appelé par thread_trampoline ---------- */
__attribute__((visibility("hidden"))) void thread_entry(void *(*func)(void *), void *arg)
{
    THREAD_COLD(self_worker->current)->started = 1;
    preempt_unblock_self();
    void *ret = func(arg);
    thread_exit(ret);
    __builtin_unreachable();
}

/* ---------- Sched stack pour worker 0 ---------- */
static void *make_main_sched_stack(void)
{
    void *base = mmap(NULL, SCHED_STACK_SIZE, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (base == MAP_FAILED) { perror("mmap sched_stack"); exit(1); }

    uintptr_t sp = (uintptr_t)base + SCHED_STACK_SIZE;
    sp &= ~(uintptr_t)0xF;
    void **s = (void **)sp;
    *(--s) = (void *)worker_loop;  /* ret addr */
    *(--s) = 0; /* rbx */
    *(--s) = 0; /* rbp */
    *(--s) = 0; /* r12 */
    *(--s) = 0; /* r13 */
    *(--s) = 0; /* r14 */
    *(--s) = 0; /* r15 */
    VALGRIND_MAKE_MEM_DEFINED(s, 7 * sizeof(void *));

    workers[0].sched_stack_base = base;
    workers[0].sched_rsp = (void *)s;
    return base;
}

/* ---------- Init / Destroy ---------- */
__attribute__((cold)) static void thread_system_destroy(void)
{
    if (system_destroyed) return;
    system_destroyed = 1;

#ifdef MN_HAS_PREEMPT
    struct itimerval off; memset(&off, 0, sizeof(off));
    setitimer(ITIMER_REAL, &off, NULL);
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGALRM);
    pthread_sigmask(SIG_BLOCK, &mask, NULL);
#endif

    runq_shutdown_broadcast();
    /* Les workers vont sortir de cond_wait, voir le shutdown, et entrer dans
     * la boucle pause() où ils ne touchent plus à libthread. On ne les
     * pthread_join pas (pthread_exit + finalisation concurrente = SIGSEGV
     * dans dlopen libgcc_s). Le kernel les tue à _exit().
     *
     * On ne libère ni la sched_stack ni les slabs : le thread courant
     * peut être en train de tourner sur n'importe lequel d'entre eux
     * (le stack_arena contient les piles user). Le kernel libère tout à _exit. */
}

__attribute__((unused)) static void mem_destroy_unused(void) { mem_destroy(); }

__attribute__((constructor, cold)) static void init_system(void)
{
    if (system_ready) return;

    mem_init();

    nworkers = 0;
    const char *env = getenv("LIBTHREAD_WORKERS");
    if (env) nworkers = atoi(env);
    if (nworkers <= 0) {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        nworkers = (n > 0) ? (int)n : 1;
    }
    if (nworkers > MAX_WORKERS) nworkers = MAX_WORKERS;

    /* Init des runqueues APRÈS détermination du nombre de workers (la table
     * de deques est dimensionnée dessus en phase 1). */
    runq_init(nworkers);

    /* Worker 0 = main pthread. */
    memset(&workers[0], 0, sizeof(workers[0]));
    workers[0].id = 0;
    workers[0].is_main = 1;
    workers[0].pthread = pthread_self();
    self_worker = &workers[0];
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu > 0) pin_worker_to_cpu(0);

    /* Allocate the main user thread descriptor. */
    thread_hot_t *main_t = thread_alloc();
    if (!main_t) { perror("thread_alloc main"); exit(1); }
    thread_cold_t *mc = THREAD_COLD(main_t);
    main_t->rsp = NULL;
    main_t->sched_next = NULL;
    main_t->sched_prev = NULL;
    thread_cold_reset(mc);
    mc->started = 1;  /* main tourne déjà */
    workers[0].current = main_t;

    /* Allocate sched stack for worker 0 (main). */
    make_main_sched_stack();

    /* Block SIGALRM on the main thread before any worker is spawned;
     * each worker starts with SIGALRM blocked (inherited mask), and unblocks itself. */
    sigset_t blockalrm, oldmask;
    sigemptyset(&blockalrm);
#ifdef MN_HAS_PREEMPT
    sigaddset(&blockalrm, SIGALRM);
    pthread_sigmask(SIG_BLOCK, &blockalrm, &oldmask);
#else
    (void)blockalrm; (void)oldmask;
#endif

    /* Spawn workers 1..N-1. They inherit our (blocked) sigmask. */
    for (int i = 1; i < nworkers; i++) {
        memset(&workers[i], 0, sizeof(workers[i]));
        workers[i].id = i;
        workers[i].is_main = 0;
        if (pthread_create(&workers[i].pthread, NULL, worker_entry, &workers[i]) != 0) {
            perror("pthread_create");
            nworkers = i;
            break;
        }
    }

    alive_count = 1;  /* the main user thread */
    atexit(thread_system_destroy);
    system_ready = 1;

#ifdef MN_HAS_PREEMPT
    preempt_init();
    /* Unblock SIGALRM on main pthread (worker 0). */
    pthread_sigmask(SIG_SETMASK, &oldmask, NULL);
    preempt_unblock_self();
#endif
}

/* ---------- Public API ---------- */
__attribute__((visibility("default"))) thread_t thread_self(void)
{
    return (thread_t)self_worker->current;
}

__attribute__((visibility("default"))) int thread_yield(void)
{
    if (__builtin_expect(self_worker == NULL, 0)) return 0;
    if (__builtin_expect(self_worker->inline_executing > 0, 0)) return 0;
#ifdef MN_HAS_PREEMPT
    sigset_t old;
    preempt_block(&old);
#endif
    flush_last_created();
    schedule();
#ifdef MN_HAS_PREEMPT
    preempt_restore(&old);
#endif
    return 0;
}

__attribute__((visibility("default")))
int thread_create(thread_t *newthread, void *(*func)(void *), void *arg)
{
#ifdef MN_HAS_PREEMPT
    sigset_t old; preempt_block(&old);
#endif
    worker_t *w = self_worker;

    thread_hot_t *t = thread_alloc();
    if (!t) {
#ifdef MN_HAS_PREEMPT
        preempt_restore(&old);
#endif
        return -1;
    }

    thread_cold_t *tc = THREAD_COLD(t);
    thread_cold_reset(tc);
    t->rsp = NULL;
    t->sched_next = NULL;
    t->sched_prev = NULL;

    /* flush previous last_created (if any), then stash current one in TLS. */
    flush_last_created();
    w->last_created = t;
    w->last_created_func = func;
    w->last_created_arg = arg;
    *newthread = (thread_t)t;

    __atomic_add_fetch(&alive_count, 1, __ATOMIC_RELAXED);

#ifdef MN_HAS_PREEMPT
    preempt_restore(&old);
#endif
    return 0;
}

/* ---- Detection deadlock (uf) ----
 * Compilation : -DLIBTHREAD_NO_DEADLOCK_DETECT pour désactiver. Le test 81
 * dépend de ce check ; le default reste activé. Désactivation = -1 spinlock
 * global pris par thread_join, gain mesurable sous forte contention join. */
#ifndef LIBTHREAD_NO_DEADLOCK_DETECT
static thread_cold_t *uf_find_locked(thread_cold_t *t)
{
    while (t->parent != t) t = t->parent = t->parent->parent;
    return t;
}

static int uf_union(thread_cold_t *a, thread_cold_t *b)
{
    pthread_spin_lock(&uf_lock);
    a = uf_find_locked(a);
    b = uf_find_locked(b);
    if (a == b) { pthread_spin_unlock(&uf_lock); return -1; }
    if (a->rank < b->rank) {
        thread_cold_t *tmp = a; a = b; b = tmp;
    }
    b->parent = a;
    if (a->rank == b->rank) a->rank++;
    pthread_spin_unlock(&uf_lock);
    return 0;
}
#else
/* Stub no-op : aucune détection de cycle, gain de spinlock global. */
static inline int uf_union(thread_cold_t *a, thread_cold_t *b)
{
    (void)a; (void)b;
    return 0;
}
#endif

/* ---- thread_join ---- */
static void unlock_cold_cb(void *arg)
{
    thread_cold_t *tc = (thread_cold_t *)arg;
    pthread_spin_unlock(&tc->cold_lock);
}

__attribute__((visibility("default"), hot)) int thread_join(thread_t thread, void **retval)
{
#ifdef MN_HAS_PREEMPT
    sigset_t old; preempt_block(&old);
#endif
    worker_t *w = self_worker;
    thread_hot_t *t = (thread_hot_t *)thread;
    thread_cold_t *tc = THREAD_COLD(t);

    /* Fast inline-join : t == last_created (TLS), pas encore visible. */
    if (w->last_created == t) {
        w->last_created = NULL;
        if (uf_union(THREAD_COLD(w->current), tc) == -1) {
            /* Le thread n'a jamais été flushé : il est orphelin (ni en TLS,
             * ni en runqueue). On libère son slot pour ne pas fuiter, et on
             * décrémente alive_count posé par thread_create. */
            w->last_created_func = NULL;
            w->last_created_arg  = NULL;
            __atomic_sub_fetch(&alive_count, 1, __ATOMIC_RELAXED);
            thread_release(t);
#ifdef MN_HAS_PREEMPT
            preempt_restore(&old);
#endif
            return EDEADLK;
        }
        void *(*fn)(void *) = w->last_created_func;
        void *ar = w->last_created_arg;
        thread_hot_t *prev = w->current;
        w->current = t;
        tc->started = 1;
        w->inline_executing++;
        jmp_buf jb;
        if (_setjmp(jb) == 0) {
            tc->inline_jmpbuf = &jb;
            tc->retval = fn(ar);
        }
        tc->inline_jmpbuf = NULL;
        tc->state = FINISHED;
        w->inline_executing--;
        w->current = prev;
        /* Le thread n'a pas appelé thread_exit (pas de longjmp) : son
         * incrément alive_count posé dans thread_create n'a jamais été
         * compensé. On le décrémente ici, sinon le compteur fuit pour
         * tout workload dominé par les fast-inline-joins. */
        __atomic_sub_fetch(&alive_count, 1, __ATOMIC_RELAXED);
        goto cleanup;
    }

    /* Tout chemin non-fast doit flusher, sinon last_created reste en TLS
     * et le thread créé n'apparaît jamais dans la runqueue.
     *
     * LIMITATION CONNUE (cross-worker fast-join) : si t a été créé par un
     * AUTRE worker (B) et est encore dans B->last_created, ce flush ne le
     * voit pas (TLS de A != TLS de B). On va donc parquer sur cold_lock et
     * attendre que B finisse par flusher (au prochain yield/create/exit
     * qu'il fera). Latence non bornée si B part en calcul long sans yield.
     * En pratique, les workloads typiques font des yields fréquents donc
     * la latence reste faible ; à reconsidérer si on observe un drop sur
     * 121/123. Fix possible : flag atomic-publié sur tc qui permet à un
     * autre worker de promouvoir t depuis B->last_created. */
    flush_last_created();

    pthread_spin_lock(&tc->cold_lock);
    if (tc->state == FINISHED) {
        pthread_spin_unlock(&tc->cold_lock);
        goto cleanup;
    }

    /* Détection de deadlock graphe d'attente. */
    if (uf_union(THREAD_COLD(w->current), tc) == -1) {
        pthread_spin_unlock(&tc->cold_lock);
#ifdef MN_HAS_PREEMPT
        preempt_restore(&old);
#endif
        return EDEADLK;
    }

    /* Note : pas d'inline-join généralisé en MN — fausse l'équité (le
     * thread inline-exécuté ignore les yields, prenant toute la CPU). */

    /* Cas général : on park sur le cold_lock. */
    tc->waiting = w->current;
    schedule_park(unlock_cold_cb, tc);
    /* Au réveil, tc->state == FINISHED. */

cleanup:
    /* Lecture de retval sous lock : garantit l'happens-before avec le store
     * fait dans thread_exit (qui n'écrit retval qu'avant de prendre le
     * cold_lock). En pratique x86 fournit déjà cet ordering, mais le rendre
     * explicite évite qu'un futur refactor ne casse l'invariant. */
    pthread_spin_lock(&tc->cold_lock);
    if (retval) *retval = tc->retval;
    uint16_t rc = --tc->refcount;
    pthread_spin_unlock(&tc->cold_lock);
    if (rc == 0) {
        if (tc->stack_base) {
#ifndef NVALGRIND
            VALGRIND_STACK_DEREGISTER(tc->valgrind_stackid);
#endif
            stack_release(tc->stack_base);
            tc->stack_base = NULL;
        }
        thread_release(t);
    }
#ifdef MN_HAS_PREEMPT
    preempt_restore(&old);
#endif
    return 0;
}

/* ---- thread_exit ---- */
static void exit_wake_waiter_cb(void *arg)
{
    runq_push((thread_hot_t *)arg);
}

__attribute__((visibility("default"), hot, __noreturn__)) void thread_exit(void *retval)
{
#ifdef MN_HAS_PREEMPT
    sigset_t old; preempt_block(&old);
    (void)old;
#endif
    worker_t *w = self_worker;
    thread_hot_t *me = w->current;
    thread_cold_t *cc = THREAD_COLD(me);
    cc->retval = retval;

    /* Fast path inline-join : longjmp vers le joineur. */
    if (cc->inline_jmpbuf != NULL) {
        _longjmp(*cc->inline_jmpbuf, 1);
        __builtin_unreachable();
    }

    /* Flush du last_created éventuel. */
    flush_last_created();

    pthread_spin_lock(&cc->cold_lock);
    cc->state = FINISHED;
    thread_hot_t *waiter_to_wake = cc->waiting;
    cc->waiting = NULL;
    pthread_spin_unlock(&cc->cold_lock);

    /* Différer le push du waiter via after_switch. Sinon, en mode multi-
     * worker avec push pouvant aller sur un autre pthread, le joineur
     * peut être popé et exécuter thread_release(me) — qui libère notre
     * stack — alors qu'on tourne encore dessus (entre runq_push et
     * schedule_exit ci-dessous). after_switch est invoqué par worker_loop
     * APRÈS le context_restore de schedule_exit, donc une fois qu'on
     * a quitté la pile utilisateur. */
    if (waiter_to_wake) {
        w->after_switch = exit_wake_waiter_cb;
        w->after_arg = waiter_to_wake;
    }

    int alive = __atomic_sub_fetch(&alive_count, 1, __ATOMIC_RELAXED);
    if (alive == 0) {
        /* Dernier thread : on déclenche la sortie process. */
        runq_shutdown_broadcast();
        exit(0);
    }
    schedule_exit();
}

/* Mutex, sémaphores, signaux : implémentations dans thread_sync.c (partagé
 * avec la variante mono). flush_last_created_pub ci-dessous expose le flush
 * statique inline pour permettre le linkage. */
__attribute__((visibility("hidden"))) void flush_last_created_pub(void)
{
    flush_last_created();
}
