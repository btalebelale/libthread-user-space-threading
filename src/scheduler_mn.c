

#include "scheduler_mn.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

__thread worker_t *self_worker = NULL;

#define MAX_RUNQS 64

#define CL_CAP 4096
#define CL_MASK (CL_CAP - 1)

typedef struct worker_runq
{

    _Alignas(64) _Atomic size_t top;

    _Alignas(64) _Atomic size_t bottom;

    _Alignas(64) _Atomic(thread_hot_t *) items[CL_CAP];

    _Alignas(64) pthread_mutex_t idle_lock;
    pthread_cond_t idle_cv;

    _Alignas(64) pthread_mutex_t inj_lock;
    thread_hot_t *inj_head;
    thread_hot_t *inj_tail;
    _Atomic int inj_size;
} __attribute__((aligned(64))) worker_runq_t;

static worker_runq_t runqs[MAX_RUNQS] __attribute__((aligned(64)));
static int n_runqs;
static volatile int runq_shutdown_flag;

static atomic_uint_fast64_t idle_bitmap;
static atomic_int push_rr;

void runq_init(int nworkers)
{
    if (nworkers > MAX_RUNQS)
        nworkers = MAX_RUNQS;
    n_runqs = nworkers;
    atomic_store_explicit(&idle_bitmap, 0, memory_order_relaxed);
    atomic_store_explicit(&push_rr, 0, memory_order_relaxed);
    for (int i = 0; i < nworkers; i++)
    {
        atomic_store_explicit(&runqs[i].top, 0, memory_order_relaxed);
        atomic_store_explicit(&runqs[i].bottom, 0, memory_order_relaxed);
        pthread_mutex_init(&runqs[i].idle_lock, NULL);
        pthread_cond_init(&runqs[i].idle_cv, NULL);
        pthread_mutex_init(&runqs[i].inj_lock, NULL);
        runqs[i].inj_head = NULL;
        runqs[i].inj_tail = NULL;
        atomic_store_explicit(&runqs[i].inj_size, 0, memory_order_relaxed);
    }
    runq_shutdown_flag = 0;
}

void runq_shutdown_broadcast(void)
{
    runq_shutdown_flag = 1;
    for (int i = 0; i < n_runqs; i++)
    {
        pthread_mutex_lock(&runqs[i].idle_lock);
        pthread_cond_broadcast(&runqs[i].idle_cv);
        pthread_mutex_unlock(&runqs[i].idle_lock);
    }
}

static inline size_t cl_push_bottom(worker_runq_t *q, thread_hot_t *t)
{
    size_t b = atomic_load_explicit(&q->bottom, memory_order_relaxed);
    size_t top = atomic_load_explicit(&q->top, memory_order_acquire);
    if (b - top >= CL_CAP)
    {
        fprintf(stderr,
                "libthread: per-worker deque full (cap=%d) — recompile with larger CL_CAP\n",
                CL_CAP);
        abort();
    }

    atomic_store_explicit(&q->items[b & CL_MASK], t, memory_order_release);
    atomic_store_explicit(&q->bottom, b + 1, memory_order_release);
    return (b + 1) - top;
}

static inline thread_hot_t *cl_pop_bottom(worker_runq_t *q)
{
    size_t b = atomic_load_explicit(&q->bottom, memory_order_relaxed) - 1;

    atomic_store_explicit(&q->bottom, b, memory_order_seq_cst);
    size_t t = atomic_load_explicit(&q->top, memory_order_seq_cst);
    if ((ptrdiff_t)(b - t) < 0)
    {

        atomic_store_explicit(&q->bottom, t, memory_order_relaxed);
        return NULL;
    }
    thread_hot_t *item = atomic_load_explicit(&q->items[b & CL_MASK], memory_order_acquire);
    if (b != t)
    {

        return item;
    }

    int won = atomic_compare_exchange_strong_explicit(&q->top, &t, t + 1, memory_order_seq_cst,
                                                      memory_order_relaxed);

    atomic_store_explicit(&q->bottom, b + 1, memory_order_relaxed);
    return won ? item : NULL;
}

static inline thread_hot_t *cl_steal(worker_runq_t *q)
{

    size_t t = atomic_load_explicit(&q->top, memory_order_seq_cst);
    size_t b = atomic_load_explicit(&q->bottom, memory_order_seq_cst);
    if ((ptrdiff_t)(b - t) <= 0)
        return NULL;
    thread_hot_t *item = atomic_load_explicit(&q->items[t & CL_MASK], memory_order_acquire);
    if (!atomic_compare_exchange_strong_explicit(&q->top, &t, t + 1, memory_order_seq_cst,
                                                 memory_order_relaxed))
    {
        return NULL;
    }
    return item;
}

static void wake_one_idle(int self_id)
{
    uint64_t idle = atomic_load_explicit(&idle_bitmap, memory_order_acquire);
    idle &= ~(1ULL << self_id);
    if (!idle)
        return;
    int v = __builtin_ctzll(idle);
    if (v >= n_runqs)
        return;
    pthread_mutex_lock(&runqs[v].idle_lock);
    pthread_cond_signal(&runqs[v].idle_cv);
    pthread_mutex_unlock(&runqs[v].idle_lock);
}

static void wake_specific(int target)
{
    if (!(atomic_load_explicit(&idle_bitmap, memory_order_acquire) & (1ULL << target)))
        return;
    pthread_mutex_lock(&runqs[target].idle_lock);
    pthread_cond_signal(&runqs[target].idle_cv);
    pthread_mutex_unlock(&runqs[target].idle_lock);
}

static void inj_push(int target, thread_hot_t *t)
{
    worker_runq_t *q = &runqs[target];
    pthread_mutex_lock(&q->inj_lock);

    t->sched_next = NULL;
    if (q->inj_tail == NULL)
        q->inj_head = t;
    else
        q->inj_tail->sched_next = t;
    q->inj_tail = t;
    atomic_fetch_add_explicit(&q->inj_size, 1, memory_order_release);
    pthread_mutex_unlock(&q->inj_lock);
    wake_specific(target);
}

static thread_hot_t *try_drain_injector(worker_runq_t *q)
{
    if (atomic_load_explicit(&q->inj_size, memory_order_acquire) == 0)
        return NULL;
    pthread_mutex_lock(&q->inj_lock);
    thread_hot_t *t = q->inj_head;
    if (t)
    {
        q->inj_head = t->sched_next;
        if (q->inj_head == NULL)
            q->inj_tail = NULL;
        atomic_fetch_sub_explicit(&q->inj_size, 1, memory_order_relaxed);
        t->sched_next = NULL;
    }
    pthread_mutex_unlock(&q->inj_lock);
    return t;
}

void runq_push(thread_hot_t *t)
{
    int self = self_worker->id;
    size_t new_size = cl_push_bottom(&runqs[self], t);
    if (new_size >= 2)
        wake_one_idle(self);
}

static thread_hot_t *try_steal(int self_id)
{
    int n = n_runqs;
    if (n <= 1)
        return NULL;

    for (int i = 1; i < n; i++)
    {
        int v = (self_id + i) % n;
        thread_hot_t *t = cl_steal(&runqs[v]);
        if (t)
            return t;
    }
    return NULL;
}

thread_hot_t *runq_pop_block(int self_id)
{
    worker_runq_t *q = &runqs[self_id];

    for (;;)
    {
        thread_hot_t *t = cl_pop_bottom(q);
        if (t)
            return t;

        t = try_drain_injector(q);
        if (t)
            return t;

        if (runq_shutdown_flag)
            return NULL;

        t = try_steal(self_id);
        if (t)
            return t;

        atomic_fetch_or_explicit(&idle_bitmap, 1ULL << self_id, memory_order_acq_rel);
        pthread_mutex_lock(&q->idle_lock);
        size_t b = atomic_load_explicit(&q->bottom, memory_order_acquire);
        size_t top = atomic_load_explicit(&q->top, memory_order_acquire);
        int inj = atomic_load_explicit(&q->inj_size, memory_order_acquire);
        if (b == top && inj == 0 && !runq_shutdown_flag)
        {
            pthread_cond_wait(&q->idle_cv, &q->idle_lock);
        }
        atomic_fetch_and_explicit(&idle_bitmap, ~(1ULL << self_id), memory_order_acq_rel);
        pthread_mutex_unlock(&q->idle_lock);
    }
}

static void schedule_push_balance_cb(void *arg)
{
    int n = n_runqs;
    int self = self_worker->id;
    int target = (int)(atomic_fetch_add_explicit(&push_rr, 1, memory_order_relaxed) % (unsigned)n);
    if (target == self)
    {
        runq_push((thread_hot_t *)arg);
    }
    else
    {
        inj_push(target, (thread_hot_t *)arg);
    }
}

void schedule(void)
{
    worker_t *worker = self_worker;
    thread_hot_t *me = worker->current;
    if (__builtin_expect(me == NULL, 0))
    {
        /* Garde-fou : ne devrait jamais arriver puisque thread_yield
         * checke current!=NULL avant. Mais si ça arrive (corruption,
         * timing pathologique), un context_switch_raw(NULL,...) crashe. */
        fprintf(stderr, "libthread: schedule() with current==NULL on worker %d, skipping\n",
                worker->id);
        return;
    }
    worker->after_switch = schedule_push_balance_cb;
    worker->after_arg = me;
    /* L'ordre des stores est CRITIQUE pour le handler SIGALRM :
     *   on_sched_stack=1 doit être visible AVANT current=NULL
     * Sinon le handler peut voir current=NULL+on_sched_stack=0 et appeler
     * thread_yield qui re-charge current=NULL → context_switch_raw(NULL).
     * compiler barrier asm volatile force l'ordre. */
    worker->on_sched_stack = 1;
    __asm__ volatile("" ::: "memory");
    worker->current = NULL;
    context_switch_raw(&me->rsp, worker->sched_rsp);
    /* Au retour, on est sur la user-stack d'un thread (potentiellement sur un
     * autre worker après migration). self_worker reflète le worker courant. */
    self_worker->on_sched_stack = 0;
}

void schedule_park(void (*cb)(void *), void *arg)
{
    worker_t *worker = self_worker;
    thread_hot_t *me = worker->current;
    if (__builtin_expect(me == NULL, 0))
    {
        fprintf(stderr, "libthread: schedule_park() with current==NULL on worker %d, skipping\n",
                worker->id);
        return;
    }
    worker->after_switch = cb;
    worker->after_arg = arg;
    /* Voir commentaire schedule() : on_sched_stack=1 doit être visible
     * AVANT current=NULL pour fermer la fenêtre de race avec le handler. */
    worker->on_sched_stack = 1;
    __asm__ volatile("" ::: "memory");
    worker->current = NULL;
    context_switch_raw(&me->rsp, worker->sched_rsp);
    self_worker->on_sched_stack = 0;
}

__attribute__((__noreturn__)) void schedule_exit(void)
{
    worker_t *worker = self_worker;
    /* on_sched_stack=1 d'abord (visible avant current=NULL) — sinon une
     * fenêtre où handler voit current=NULL+on_sched_stack=0 ; thread_yield
     * checke current et return, OK, mais autant fermer cohéremment. */
    worker->on_sched_stack = 1;
    __asm__ volatile("" ::: "memory");
    worker->current = NULL;
    context_restore_raw(worker->sched_rsp);
}

__attribute__((__noreturn__)) void worker_loop(void)
{
    worker_t *worker = self_worker;
    worker->on_sched_stack = 1;
    for (;;)
    {
        if (worker->after_switch)
        {
            void (*cb)(void *) = worker->after_switch;
            void *arg = worker->after_arg;
            worker->after_switch = NULL;
            worker->after_arg = NULL;
            cb(arg);
        }
        thread_hot_t *t = runq_pop_block(worker->id);
        if (!t)
        {
            for (;;)
                pause();
        }
        worker->current = t;
        if (__builtin_expect(t->rsp == NULL, 0))
            lazy_stack_alloc(t);
        /* on_sched_stack reste à 1 : le code user baissera le flag à 0 dès
         * qu'il aura repris (thread_entry pour un démarrage neuf, retour de
         * schedule()/schedule_park() pour une reprise). */
        context_switch_raw(&worker->sched_rsp, t->rsp);
        /* Retour : un thread a yielded ou parqué. On est de retour sur la
         * sched-stack, le flag a pu être baissé par le code user qui tournait
         * — on le remonte. */
        worker->on_sched_stack = 1;
    }
}
