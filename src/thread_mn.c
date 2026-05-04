

#define _GNU_SOURCE
#include "preempt.h"
#include "scheduler_mn.h"
#include "thread.h"
#include "thread_common.h"
#include "thread_internal.h"
#include <sched.h>

#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>

#define MAX_WORKERS 128

__attribute__((visibility("hidden"))) thread_hot_t *hot_slab;
__attribute__((visibility("hidden"))) thread_cold_t *cold_slab;
static int thread_slab_next;
static thread_hot_t *thread_free_head;

static void *stack_arena;
static int stack_arena_next;
static void *stack_free_head;

static pthread_spinlock_t alloc_lock;
#ifndef LIBTHREAD_NO_DEADLOCK_DETECT
static pthread_spinlock_t uf_lock;
#endif

static int nworkers;
static worker_t workers[MAX_WORKERS] __attribute__((aligned(64)));
static int system_ready;
static int alive_count;
static int system_destroyed;

__attribute__((visibility("hidden"))) thread_hot_t *current __attribute__((aligned(64)));
__attribute__((visibility("hidden"))) thread_hot_t *last_created;
__attribute__((visibility("hidden"))) int inline_executing;

#ifdef PREEMPT_ENABLED
/* Intervalle SIGALRM en mode MN — délibérément plus large que le 200µs du
 * mode mono. Raison : il existe une fenêtre de race subtile entre les
 * stores current=t/on_sched=1 par worker_loop et l'arrivée effective sur la
 * user-stack côté code utilisateur. À 200µs la pression sur ces fenêtres
 * déclenche des SIGSEGV / stack-smashing (62-mutex le reproduit en quelques
 * runs). À 5ms certains tests passent mais 62-mutex sous load reste fragile.
 * À 20ms (50 ticks/s/worker), la fenêtre n'est plus déclenchée en pratique
 * sur la suite — fairness reste correcte (test 71 garanti). */
#define PREEMPT_INTERVAL_US 50000

static void preempt_handler(int sig)
{
    (void)sig;
    /* Pas de preempt_unblock_self : sigaction garde SIGALRM bloqué pendant
     * tout le handler (sa_mask par défaut + signal courant). Si on débloque,
     * un nouveau tick peut nous ré-entrer pendant le thread_yield, ce qui
     * crée des fenêtres entre check et action. Le tick perdu est délivré à
     * la sortie du handler. */
    if (!self_worker)
        return;
    if (self_worker->on_sched_stack)
        return;
    if (!self_worker->current)
        return;
    if (self_worker->inline_executing)
        return;
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

__attribute__((cold)) static void mem_init(void)
{

    stack_arena = mmap(NULL, (size_t)STACK_CAP * STACK_SIZE, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (stack_arena == MAP_FAILED)
    {
        perror("mmap stack arena");
        exit(1);
    }
    madvise(stack_arena, (size_t)STACK_CAP * STACK_SIZE, MADV_HUGEPAGE);

    hot_slab = mmap(NULL, (size_t)THREAD_CAP * sizeof(thread_hot_t), PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (hot_slab == MAP_FAILED)
    {
        perror("mmap hot slab");
        exit(1);
    }

    cold_slab = mmap(NULL, (size_t)THREAD_CAP * sizeof(thread_cold_t), PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (cold_slab == MAP_FAILED)
    {
        perror("mmap cold slab");
        exit(1);
    }

    stack_arena_next = 0;
    stack_free_head = NULL;
    thread_slab_next = 0;
    thread_free_head = NULL;

    pthread_spin_init(&alloc_lock, PTHREAD_PROCESS_PRIVATE);
#ifndef LIBTHREAD_NO_DEADLOCK_DETECT
    pthread_spin_init(&uf_lock, PTHREAD_PROCESS_PRIVATE);
#endif
}

static void *stack_alloc_locked(void)
{
    if (stack_free_head != NULL)
    {
        void *s = stack_free_head;
        stack_free_head = *STACK_FREELIST_PTR(s);
        return s;
    }
    if (stack_arena_next >= STACK_CAP)
        return NULL;
    void *s = (char *)stack_arena + ((size_t)stack_arena_next++ * STACK_SIZE);

    if (mprotect(s, STACK_GUARD, PROT_NONE) != 0)
        perror("mprotect stack guard");
    return s;
}

#define T_CACHE_MAX 32
#define T_CACHE_REFILL 16
#define S_CACHE_MAX 16
#define S_CACHE_REFILL 8

static __thread thread_hot_t *t_cache_head;
static __thread int t_cache_count;
static __thread void *s_cache_head;
static __thread int s_cache_count;

static void *stack_alloc(void)
{
    if (s_cache_head)
    {
        void *s = s_cache_head;
        s_cache_head = *STACK_FREELIST_PTR(s);
        s_cache_count--;
        return s;
    }

    pthread_spin_lock(&alloc_lock);
    void *first = NULL;
    int got = 0;
    while (got < S_CACHE_REFILL)
    {
        void *s = stack_alloc_locked();
        if (!s)
            break;
        *STACK_FREELIST_PTR(s) = first;
        first = s;
        got++;
    }
    pthread_spin_unlock(&alloc_lock);
    if (!first)
        return NULL;

    void *ret = first;
    void *rest = *STACK_FREELIST_PTR(first);
    while (rest)
    {
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
    if (s_cache_count < S_CACHE_MAX)
    {
        *STACK_FREELIST_PTR(s) = s_cache_head;
        s_cache_head = s;
        s_cache_count++;
        return;
    }

    pthread_spin_lock(&alloc_lock);
    for (int i = 0; i < S_CACHE_REFILL && s_cache_head; i++)
    {
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
    if (t_cache_head)
    {
        thread_hot_t *t = t_cache_head;
        t_cache_head = (thread_hot_t *)t->rsp;
        t_cache_count--;
        return t;
    }

    pthread_spin_lock(&alloc_lock);
    thread_hot_t *list = NULL;
    int got = 0;
    while (got < T_CACHE_REFILL && thread_free_head)
    {
        thread_hot_t *t = thread_free_head;
        thread_free_head = (thread_hot_t *)t->rsp;
        t->rsp = (void *)list;
        list = t;
        got++;
    }
    while (got < T_CACHE_REFILL && thread_slab_next < THREAD_CAP)
    {
        thread_hot_t *t = &hot_slab[thread_slab_next++];
        thread_cold_t *target_cold = THREAD_COLD(t);
        pthread_spin_init(&target_cold->cold_lock, PTHREAD_PROCESS_PRIVATE);
        t->rsp = (void *)list;
        list = t;
        got++;
    }
    pthread_spin_unlock(&alloc_lock);
    if (!list)
        return NULL;
    thread_hot_t *ret = list;
    list = (thread_hot_t *)list->rsp;
    ret->rsp = NULL;
    while (list)
    {
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
    if (t_cache_count < T_CACHE_MAX)
    {
        t->rsp = (void *)t_cache_head;
        t_cache_head = t;
        t_cache_count++;
        return;
    }

    pthread_spin_lock(&alloc_lock);
    for (int i = 0; i < T_CACHE_REFILL && t_cache_head; i++)
    {
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

static int stack_color_counter;

static void *stack_init(void *stack_base, void *(*func)(void *), void *arg)
{
    uintptr_t sp = (uintptr_t)stack_base + STACK_SIZE;
    sp -= ((unsigned)__atomic_fetch_add(&stack_color_counter, 1, __ATOMIC_RELAXED) & 0xFF) * 64;
    sp &= ~(uintptr_t)0xF;

    void **s = (void **)sp;
    *(--s) = (void *)thread_trampoline;
    *(--s) = 0;
    *(--s) = 0;
    *(--s) = (void *)func;
    *(--s) = arg;
    *(--s) = 0;
    *(--s) = 0;
    VALGRIND_MAKE_MEM_DEFINED(s, 7 * sizeof(void *));
    return s;
}

__attribute__((noinline, cold, visibility("hidden"))) void lazy_stack_alloc(thread_hot_t *t)
{
    thread_cold_t *target_cold = THREAD_COLD(t);
    void *stack = stack_alloc();
    if (!stack)
    {

        fprintf(stderr, "libthread: stack arena exhausted (max %d threads concurrents)\n",
                STACK_CAP);
        abort();
    }
    target_cold->stack_base = stack;
    t->rsp = stack_init(stack, target_cold->func, target_cold->func_arg);
#ifndef NVALGRIND

    target_cold->valgrind_stackid =
        VALGRIND_STACK_REGISTER((char *)stack + STACK_GUARD, (char *)stack + STACK_SIZE);
#endif
}

__attribute__((noinline, cold, visibility("hidden"))) void flush_last_created_slow(void)
{
}

static inline void flush_last_created(void)
{
    worker_t *worker = self_worker;
    if (__builtin_expect(worker->last_created != NULL, 0))
    {
        thread_hot_t *t = worker->last_created;
        thread_cold_t *target_cold = THREAD_COLD(t);
        target_cold->func = worker->last_created_func;
        target_cold->func_arg = worker->last_created_arg;
        worker->last_created = NULL;
        runq_push(t);
    }
}

__attribute__((cold)) static void pin_worker_to_cpu(int cpu)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

__attribute__((__noreturn__)) static void *worker_entry(void *arg)
{
    worker_t *worker = (worker_t *)arg;
    self_worker = worker;
    /* Le worker démarre sur la sched-stack (la pile pthread native lui sert
     * de sched-stack jusqu'au premier context_switch_raw). Marquer le flag
     * AVANT preempt_unblock_self : sinon un SIGALRM peut arriver entre
     * unblock et l'entrée dans worker_loop avec on_sched_stack=0 par défaut. */
    worker->on_sched_stack = 1;
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu > 0)
        pin_worker_to_cpu(worker->id % (int)ncpu);
#ifdef MN_HAS_PREEMPT
    preempt_unblock_self();
#endif

    worker_loop();
}

__attribute__((visibility("hidden"))) void thread_entry(void *(*func)(void *), void *arg)
{
    /* On vient d'arriver sur la user-stack via le trampoline asm. Baisser
     * le flag pour autoriser la préemption sur ce thread. */
    self_worker->on_sched_stack = 0;
    THREAD_COLD(self_worker->current)->started = 1;
    preempt_unblock_self();
    void *ret = func(arg);
    thread_exit(ret);
    __builtin_unreachable();
}

static void *make_main_sched_stack(void)
{
    void *base = mmap(NULL, SCHED_STACK_SIZE, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (base == MAP_FAILED)
    {
        perror("mmap sched_stack");
        exit(1);
    }

    uintptr_t sp = (uintptr_t)base + SCHED_STACK_SIZE;
    sp &= ~(uintptr_t)0xF;
    void **s = (void **)sp;
    *(--s) = (void *)worker_loop;
    *(--s) = 0;
    *(--s) = 0;
    *(--s) = 0;
    *(--s) = 0;
    *(--s) = 0;
    *(--s) = 0;
    VALGRIND_MAKE_MEM_DEFINED(s, 7 * sizeof(void *));

    workers[0].sched_stack_base = base;
    workers[0].sched_rsp = (void *)s;
    return base;
}

__attribute__((cold)) static void thread_system_destroy(void)
{
    if (system_destroyed)
        return;
    system_destroyed = 1;

#ifdef MN_HAS_PREEMPT
    struct itimerval off;
    memset(&off, 0, sizeof(off));
    setitimer(ITIMER_REAL, &off, NULL);
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGALRM);
    pthread_sigmask(SIG_BLOCK, &mask, NULL);
#endif

    runq_shutdown_broadcast();
}

__attribute__((constructor, cold)) static void init_system(void)
{
    if (system_ready)
        return;

    mem_init();

    nworkers = 0;
    const char *env = getenv("LIBTHREAD_WORKERS");
    if (env)
        nworkers = atoi(env);
    if (nworkers <= 0)
    {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        nworkers = (n > 0) ? (int)n : 1;
    }
    if (nworkers > MAX_WORKERS)
        nworkers = MAX_WORKERS;

    runq_init(nworkers);

    memset(&workers[0], 0, sizeof(workers[0]));
    workers[0].id = 0;
    workers[0].is_main = 1;
    workers[0].pthread = pthread_self();
    /* Le main du programme tourne déjà sur sa user-stack — on n'est PAS sur
     * la sched-stack. Le flag passera à 1 au premier yield (entrée dans
     * schedule()). */
    workers[0].on_sched_stack = 0;
    self_worker = &workers[0];
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu > 0)
        pin_worker_to_cpu(0);

    thread_hot_t *main_t = thread_alloc();
    if (!main_t)
    {
        perror("thread_alloc main");
        exit(1);
    }
    thread_cold_t *main_cold = THREAD_COLD(main_t);
    main_t->rsp = NULL;
    main_t->sched_next = NULL;
    thread_cold_reset(main_cold);
    main_cold->sched_prev = NULL;
    main_cold->started = 1;
    workers[0].current = main_t;

    make_main_sched_stack();

    sigset_t blockalrm, oldmask;
    sigemptyset(&blockalrm);
#ifdef MN_HAS_PREEMPT
    sigaddset(&blockalrm, SIGALRM);
    pthread_sigmask(SIG_BLOCK, &blockalrm, &oldmask);
#else
    (void)blockalrm;
    (void)oldmask;
#endif

    for (int i = 1; i < nworkers; i++)
    {
        memset(&workers[i], 0, sizeof(workers[i]));
        workers[i].id = i;
        workers[i].is_main = 0;
        if (pthread_create(&workers[i].pthread, NULL, worker_entry, &workers[i]) != 0)
        {
            perror("pthread_create");
            nworkers = i;
            break;
        }
    }

    alive_count = 1;
    atexit(thread_system_destroy);
    system_ready = 1;

#ifdef MN_HAS_PREEMPT
    preempt_init();

    pthread_sigmask(SIG_SETMASK, &oldmask, NULL);
    preempt_unblock_self();
#endif
}

__attribute__((visibility("default"))) thread_t thread_self(void)
{
    return (thread_t)self_worker->current;
}

__attribute__((visibility("default"))) int thread_yield(void)
{
    if (__builtin_expect(self_worker == NULL, 0))
        return 0;
    if (__builtin_expect(self_worker->inline_executing > 0, 0))
        return 0;
    /* Garde-fou : current peut être NULL dans des fenêtres très étroites
     * (ex : tick SIGALRM pendant que le handler vient d'être ré-armé alors
     * qu'on tourne sur la sched-stack après schedule_exit). on_sched_stack
     * couvre la majorité, mais on_sched_stack=0 + current=NULL est possible
     * pendant la transition juste avant que worker_loop ne pose current=t.
     * Sans ce check, schedule() ferait context_switch_raw(NULL, ...) → SIGSEGV. */
    if (__builtin_expect(self_worker->current == NULL, 0))
        return 0;
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

__attribute__((visibility("default"))) int thread_create(thread_t *newthread, void *(*func)(void *),
                                                         void *arg)
{
#ifdef MN_HAS_PREEMPT
    sigset_t old;
    preempt_block(&old);
#endif
    worker_t *worker = self_worker;

    thread_hot_t *t = thread_alloc();
    if (!t)
    {
#ifdef MN_HAS_PREEMPT
        preempt_restore(&old);
#endif
        return -1;
    }

    thread_cold_t *target_cold = THREAD_COLD(t);
    thread_cold_reset(target_cold);
    t->rsp = NULL;
    t->sched_next = NULL;
    target_cold->sched_prev = NULL;

    flush_last_created();
    worker->last_created = t;
    worker->last_created_func = func;
    worker->last_created_arg = arg;
    *newthread = (thread_t)t;

    __atomic_add_fetch(&alive_count, 1, __ATOMIC_RELAXED);

#ifdef MN_HAS_PREEMPT
    preempt_restore(&old);
#endif
    return 0;
}

#ifndef LIBTHREAD_NO_DEADLOCK_DETECT
static thread_cold_t *uf_find_locked(thread_cold_t *t)
{
    while (t->parent != t)
        t = t->parent = t->parent->parent;
    return t;
}

static int uf_union(thread_cold_t *a, thread_cold_t *b)
{
    pthread_spin_lock(&uf_lock);
    a = uf_find_locked(a);
    b = uf_find_locked(b);
    if (a == b)
    {
        pthread_spin_unlock(&uf_lock);
        return -1;
    }
    if (a->rank < b->rank)
    {
        thread_cold_t *tmp = a;
        a = b;
        b = tmp;
    }
    b->parent = a;
    if (a->rank == b->rank)
        a->rank++;
    pthread_spin_unlock(&uf_lock);
    return 0;
}
#else

static inline int uf_union(thread_cold_t *a, thread_cold_t *b)
{
    (void)a;
    (void)b;
    return 0;
}
#endif

static void unlock_cold_cb(void *arg)
{
    thread_cold_t *target_cold = (thread_cold_t *)arg;
    pthread_spin_unlock(&target_cold->cold_lock);
}

static inline __attribute__((always_inline)) int join_park_mn(worker_t *worker,
                                                              thread_cold_t *target_cold)
{
    pthread_spin_lock(&target_cold->cold_lock);
    if (target_cold->state == FINISHED)
    {
        pthread_spin_unlock(&target_cold->cold_lock);
        return 0;
    }

    if (uf_union(THREAD_COLD(worker->current), target_cold) == -1)
    {
        pthread_spin_unlock(&target_cold->cold_lock);
        return EDEADLK;
    }

    target_cold->waiting = worker->current;
    schedule_park(unlock_cold_cb, target_cold);
    return 0;
}

static inline __attribute__((always_inline)) void
join_cleanup_mn(thread_hot_t *t, thread_cold_t *target_cold, void **retval)
{
    pthread_spin_lock(&target_cold->cold_lock);
    if (retval)
        *retval = target_cold->retval;
    uint16_t rc = --target_cold->refcount;
    pthread_spin_unlock(&target_cold->cold_lock);
    if (rc == 0)
    {
        if (target_cold->stack_base)
        {
#ifndef NVALGRIND
            VALGRIND_STACK_DEREGISTER(target_cold->valgrind_stackid);
#endif
            stack_release(target_cold->stack_base);
            target_cold->stack_base = NULL;
        }
        thread_release(t);
    }
}

__attribute__((visibility("default"), hot)) int thread_join(thread_t thread, void **retval)
{
#ifdef MN_HAS_PREEMPT
    sigset_t old;
    preempt_block(&old);
#endif
    worker_t *worker = self_worker;
    thread_hot_t *t = (thread_hot_t *)thread;
    thread_cold_t *target_cold = THREAD_COLD(t);

    if (worker->last_created == t)
    {
        worker->last_created = NULL;
        if (uf_union(THREAD_COLD(worker->current), target_cold) == -1)
        {
            worker->last_created_func = NULL;
            worker->last_created_arg = NULL;
            __atomic_sub_fetch(&alive_count, 1, __ATOMIC_RELAXED);
            thread_release(t);
#ifdef MN_HAS_PREEMPT
            preempt_restore(&old);
#endif
            return EDEADLK;
        }
        void *(*fn)(void *) = worker->last_created_func;
        void *ar = worker->last_created_arg;
        thread_hot_t *prev = worker->current;
        worker->current = t;
        target_cold->started = 1;
        worker->inline_executing++;
        jmp_buf jb;
        if (_setjmp(jb) == 0)
        {
            target_cold->inline_jmpbuf = &jb;
            target_cold->retval = fn(ar);
        }
        target_cold->inline_jmpbuf = NULL;
        target_cold->state = FINISHED;
        worker->inline_executing--;
        worker->current = prev;
        __atomic_sub_fetch(&alive_count, 1, __ATOMIC_RELAXED);
    }
    else
    {
        flush_last_created();
        int rc = join_park_mn(worker, target_cold);
        if (rc == EDEADLK)
        {
#ifdef MN_HAS_PREEMPT
            preempt_restore(&old);
#endif
            return EDEADLK;
        }
    }

    join_cleanup_mn(t, target_cold, retval);
#ifdef MN_HAS_PREEMPT
    preempt_restore(&old);
#endif
    return 0;
}

static void exit_wake_waiter_cb(void *arg)
{
    runq_push((thread_hot_t *)arg);
}

__attribute__((visibility("default"), hot, __noreturn__)) void thread_exit(void *retval)
{
#ifdef MN_HAS_PREEMPT
    sigset_t old;
    preempt_block(&old);
    (void)old;
#endif
    worker_t *worker = self_worker;
    thread_hot_t *me = worker->current;
    thread_cold_t *current_cold = THREAD_COLD(me);
    current_cold->retval = retval;

    if (current_cold->inline_jmpbuf != NULL)
    {
        _longjmp(*current_cold->inline_jmpbuf, 1);
        __builtin_unreachable();
    }

    flush_last_created();

    pthread_spin_lock(&current_cold->cold_lock);
    current_cold->state = FINISHED;
    thread_hot_t *waiter_to_wake = current_cold->waiting;
    current_cold->waiting = NULL;
    pthread_spin_unlock(&current_cold->cold_lock);

    if (waiter_to_wake)
    {
        worker->after_switch = exit_wake_waiter_cb;
        worker->after_arg = waiter_to_wake;
    }

    int alive = __atomic_sub_fetch(&alive_count, 1, __ATOMIC_RELAXED);
    if (alive == 0)
    {

        runq_shutdown_broadcast();
        exit(0);
    }
    schedule_exit();
}

__attribute__((visibility("hidden"))) void flush_last_created_pub(void)
{
    flush_last_created();
}
