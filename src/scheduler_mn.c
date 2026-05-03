/* Scheduler M:N — Phase 2 : Chase-Lev SPMC lock-free per-worker deque,
 * work-stealing lock-free, et yield RR cross-worker via injector queue
 * (mutex-protégée, drainée par l'owner).
 *
 * Architecture
 * -------------
 * Une deque par worker en ring buffer (Chase-Lev 2005 ; correctifs Lê,
 * Pop, Cohen, Nardelli 2013 pour les memory orderings sur modèles
 * faibles). Owner = seul producteur sur cette deque (push/pop bottom) ;
 * voleurs en CAS sur top.
 *
 * Push externe (yield RR vers un autre worker) : injector queue
 * mutex-protégée par target. Owner draine 1 item à chaque pop_block.
 * Le coût mutex est payé par les yields cross-worker (peu fréquents).
 *
 * Wake-up idle : un mutex+cv per-worker (idle_lock/idle_cv), distinct
 * de la deque ring. idle_bitmap atomique : annonce avant cond_wait.
 *
 * Synchronisation pour le push différé en sortie (thread_exit) : voir
 * exit_wake_waiter_cb dans thread_mn.c. Sans ce différé, le joineur
 * peut être popé par un autre worker, libérer la stack du thread sortant
 * alors que celui-ci tourne encore dessus entre runq_push et
 * schedule_exit.
 *
 * Bug critique trouvé et corrigé pendant cette phase
 * ---------------------------------------------------
 * Dans cl_pop_bottom, le path "dernier item, race avec voleur via CAS
 * sur top" restaurait incorrectement bottom à `t + 1` après échec du
 * CAS. Or `compare_exchange_strong_explicit` met à jour `t` avec la
 * valeur actuelle de top en cas d'échec (= old_top + 1). Donc on
 * stockait bottom = old_top + 2 au lieu de b + 1 = old_top + 1. La
 * deque entrait dans un état (bottom > top) avec un item fantôme à
 * items[(b+1) & MASK], slot non écrit récemment. Le prochain pop ou
 * steal lisait un pointeur stale et crashait. Manifestation : SEGV
 * aléatoire sur 62-mutex (yield dans section critique mutex, qui
 * exposait beaucoup de configurations 1-item) et 107.
 *
 * Fix : utiliser b + 1 (constante connue) au lieu de t + 1 (variable
 * réécrite par le CAS). Voir commentaire inline dans cl_pop_bottom. */

#include "scheduler_mn.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdio.h>
#include <unistd.h>

__thread worker_t *self_worker = NULL;

#define MAX_RUNQS 64

/* Capacité ring buffer (puissance de 2). 4096 slots × 8 octets = 32 Kio
 * par worker — confortable pour les workloads testés (pic ~200 threads
 * par worker sur 21/22/23). Si dépassement : abort propre. */
#define CL_CAP   4096
#define CL_MASK  (CL_CAP - 1)

typedef struct worker_runq {
    /* top sur sa propre cache-line : tous les voleurs CAS dessus. */
    _Alignas(64) _Atomic size_t top;
    /* bottom sur sa propre cache-line : owner écrit, voleurs lisent. */
    _Alignas(64) _Atomic size_t bottom;

    /* Ring buffer : items[i & MASK] = pointeur thread. */
    _Alignas(64) _Atomic(thread_hot_t *) items[CL_CAP];

    /* Idle-sleep : utilisé UNIQUEMENT pour cond_wait quand la deque est
     * vide et aucun vol n'a abouti. Ne protège PAS la deque ring. */
    _Alignas(64) pthread_mutex_t idle_lock;
    pthread_cond_t  idle_cv;

    /* Injector queue : push externes (yield RR cross-worker). Linked
     * list mutex-protégée. L'owner draine 1 item par appel à pop_block. */
    _Alignas(64) pthread_mutex_t inj_lock;
    thread_hot_t   *inj_head;
    thread_hot_t   *inj_tail;
    _Atomic int     inj_size;
} __attribute__((aligned(64))) worker_runq_t;

static worker_runq_t runqs[MAX_RUNQS] __attribute__((aligned(64)));
static int n_runqs;
static volatile int runq_shutdown_flag;

static atomic_uint_fast64_t idle_bitmap;
static atomic_int push_rr;

void runq_init(int nworkers)
{
    if (nworkers > MAX_RUNQS) nworkers = MAX_RUNQS;
    n_runqs = nworkers;
    atomic_store_explicit(&idle_bitmap, 0, memory_order_relaxed);
    atomic_store_explicit(&push_rr, 0, memory_order_relaxed);
    for (int i = 0; i < nworkers; i++) {
        atomic_store_explicit(&runqs[i].top,    0, memory_order_relaxed);
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

void runq_destroy(void) {}

void runq_shutdown_broadcast(void)
{
    runq_shutdown_flag = 1;
    for (int i = 0; i < n_runqs; i++) {
        pthread_mutex_lock(&runqs[i].idle_lock);
        pthread_cond_broadcast(&runqs[i].idle_cv);
        pthread_mutex_unlock(&runqs[i].idle_lock);
    }
}

/* ---------- Chase-Lev primitives ---------- */

/* Owner push. Single-producer. Renvoie taille approximative après push. */
static inline size_t cl_push_bottom(worker_runq_t *q, thread_hot_t *t)
{
    size_t b = atomic_load_explicit(&q->bottom, memory_order_relaxed);
    size_t top = atomic_load_explicit(&q->top, memory_order_acquire);
    if (b - top >= CL_CAP) {
        fprintf(stderr,
                "libthread: per-worker deque full (cap=%d) — recompile with larger CL_CAP\n",
                CL_CAP);
        abort();
    }
    /* items store release : le slot doit être visible AVANT bottom store
     * pour les voleurs qui font load(top) puis fence puis load(bottom). */
    atomic_store_explicit(&q->items[b & CL_MASK], t, memory_order_release);
    atomic_store_explicit(&q->bottom, b + 1, memory_order_release);
    return (b + 1) - top;
}

/* Owner pop bottom. Race avec voleurs sur le dernier item : CAS sur top. */
static inline thread_hot_t *cl_pop_bottom(worker_runq_t *q)
{
    size_t b = atomic_load_explicit(&q->bottom, memory_order_relaxed) - 1;
    /* store seq_cst sur bottom : empêche le réordonnancement avec le
     * load(top) qui suit (sans cela, voleur pourrait voir bottom inchangé
     * et voler simultanément l'item qu'on est en train de pop). */
    atomic_store_explicit(&q->bottom, b, memory_order_seq_cst);
    size_t t = atomic_load_explicit(&q->top, memory_order_seq_cst);
    if ((ptrdiff_t)(b - t) < 0) {
        /* Vide. Restaurer bottom et sortir. */
        atomic_store_explicit(&q->bottom, t, memory_order_relaxed);
        return NULL;
    }
    thread_hot_t *item = atomic_load_explicit(&q->items[b & CL_MASK],
                                              memory_order_acquire);
    if (b != t) {
        /* >= 2 items : pas de race possible (voleur prend top, on prend
         * bottom). Pas besoin de CAS. */
        return item;
    }
    /* Dernier item : un voleur peut être en train d'incrémenter top.
     * CAS sur top : si on gagne, on a l'item ; sinon, le voleur l'a pris. */
    int won = atomic_compare_exchange_strong_explicit(
        &q->top, &t, t + 1,
        memory_order_seq_cst, memory_order_relaxed);
    /* CRITIQUE : restaurer bottom à b + 1 (la valeur logique post-CAS,
     * top = b + 1 dans les deux cas), PAS t + 1. compare_exchange_strong
     * met à jour `t` avec la valeur actuelle si le CAS échoue : t
     * deviendrait b + 1, donc t + 1 = b + 2, ce qui laisserait
     * (top = b + 1, bottom = b + 2) — un état "1 item fantôme" avec
     * items[(b+1) & MASK] non-écrit. Bug Chase-Lev classique. */
    atomic_store_explicit(&q->bottom, b + 1, memory_order_relaxed);
    return won ? item : NULL;
}

/* Voleur. Tente de prendre l'item au top. Renvoie NULL si vide ou perdu CAS. */
static inline thread_hot_t *cl_steal(worker_runq_t *q)
{
    /* seq_cst sur top et bottom : ordering total avec les stores seq_cst
     * de cl_pop_bottom. Sans cet ordre, voleur pourrait voir un bottom
     * récent (post pop_bottom) et un top obsolète (pré CAS), prenant un
     * item fantôme. */
    size_t t = atomic_load_explicit(&q->top, memory_order_seq_cst);
    size_t b = atomic_load_explicit(&q->bottom, memory_order_seq_cst);
    if ((ptrdiff_t)(b - t) <= 0) return NULL;
    thread_hot_t *item = atomic_load_explicit(&q->items[t & CL_MASK],
                                              memory_order_acquire);
    if (!atomic_compare_exchange_strong_explicit(
            &q->top, &t, t + 1,
            memory_order_seq_cst, memory_order_relaxed)) {
        return NULL;
    }
    return item;
}

/* ---------- Wake-up idle ---------- */

static void wake_one_idle(int self_id)
{
    uint64_t idle = atomic_load_explicit(&idle_bitmap, memory_order_acquire);
    idle &= ~(1ULL << self_id);
    if (!idle) return;
    int v = __builtin_ctzll(idle);
    if (v >= n_runqs) return;
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

/* ---------- Injector queue ---------- */

/* Push externe : appelé par worker A pour pousser un thread sur la
 * runqueue de B (B != A). Mutex-protégé. Yield RR uniquement. */
static void inj_push(int target, thread_hot_t *t)
{
    worker_runq_t *q = &runqs[target];
    pthread_mutex_lock(&q->inj_lock);
    /* Reset sched_next : peut être stale (pointe vers un voisin d'une
     * ancienne wait-list mutex/sem). */
    t->sched_next = NULL;
    if (q->inj_tail == NULL) q->inj_head = t;
    else q->inj_tail->sched_next = t;
    q->inj_tail = t;
    atomic_fetch_add_explicit(&q->inj_size, 1, memory_order_release);
    pthread_mutex_unlock(&q->inj_lock);
    wake_specific(target);
}

/* Drain-one : pop un seul item de la inj queue. Évite d'interagir avec
 * le ring buffer de la deque, donc pas de race entre drain et voleurs. */
static thread_hot_t *try_drain_injector(worker_runq_t *q)
{
    if (atomic_load_explicit(&q->inj_size, memory_order_acquire) == 0)
        return NULL;
    pthread_mutex_lock(&q->inj_lock);
    thread_hot_t *t = q->inj_head;
    if (t) {
        q->inj_head = t->sched_next;
        if (q->inj_head == NULL) q->inj_tail = NULL;
        atomic_fetch_sub_explicit(&q->inj_size, 1, memory_order_relaxed);
        t->sched_next = NULL;
    }
    pthread_mutex_unlock(&q->inj_lock);
    return t;
}

/* ---------- API publique ---------- */

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
    if (n <= 1) return NULL;
    /* En Chase-Lev pure, cl_steal CAS-based prend size=1 sans risque :
     * l'heuristique "steal-when-busy" du design lock-protected
     * (consultation de workers[v].current) n'est pas nécessaire. */
    for (int i = 1; i < n; i++) {
        int v = (self_id + i) % n;
        thread_hot_t *t = cl_steal(&runqs[v]);
        if (t) return t;
    }
    return NULL;
}

thread_hot_t *runq_pop_block(int self_id)
{
    worker_runq_t *q = &runqs[self_id];

    for (;;) {
        thread_hot_t *t = cl_pop_bottom(q);
        if (t) return t;

        t = try_drain_injector(q);
        if (t) return t;

        if (runq_shutdown_flag) return NULL;

        t = try_steal(self_id);
        if (t) return t;

        atomic_fetch_or_explicit(&idle_bitmap, 1ULL << self_id,
                                 memory_order_acq_rel);
        pthread_mutex_lock(&q->idle_lock);
        size_t b = atomic_load_explicit(&q->bottom, memory_order_acquire);
        size_t top = atomic_load_explicit(&q->top, memory_order_acquire);
        int inj = atomic_load_explicit(&q->inj_size, memory_order_acquire);
        if (b == top && inj == 0 && !runq_shutdown_flag) {
            pthread_cond_wait(&q->idle_cv, &q->idle_lock);
        }
        atomic_fetch_and_explicit(&idle_bitmap, ~(1ULL << self_id),
                                  memory_order_acq_rel);
        pthread_mutex_unlock(&q->idle_lock);
    }
}

/* Push différé du thread sortant : exécuté par worker_loop APRÈS le
 * context_switch (donc une fois l'owner descendu de la pile user de me).
 * Sans ce différé, un voleur peut prendre me et un autre worker peut
 * libérer sa stack avant que le context_switch n'ait fini. Cf phase1.txt
 * (bug stack-reuse de thread_exit).
 *
 * Yield RR : pousse round-robin sur les workers. Si target == self,
 * push local Chase-Lev ; sinon, push externe via injector queue. Force
 * la migration des threads yield-bound, indispensable pour 03 et 107. */
static void schedule_push_balance_cb(void *arg)
{
    int n = n_runqs;
    int self = self_worker->id;
    int target = (int)(atomic_fetch_add_explicit(&push_rr, 1,
                                                 memory_order_relaxed)
                       % (unsigned)n);
    if (target == self) {
        runq_push((thread_hot_t *)arg);
    } else {
        inj_push(target, (thread_hot_t *)arg);
    }
}

void schedule(void)
{
    worker_t *w = self_worker;
    thread_hot_t *me = w->current;
    w->current = NULL;
    w->after_switch = schedule_push_balance_cb;
    w->after_arg = me;
    context_switch(&me->rsp, w->sched_rsp);
}

void schedule_park(void (*cb)(void *), void *arg)
{
    worker_t *w = self_worker;
    thread_hot_t *me = w->current;
    w->current = NULL;
    w->after_switch = cb;
    w->after_arg = arg;
    context_switch(&me->rsp, w->sched_rsp);
}

__attribute__((__noreturn__)) void schedule_exit(void)
{
    worker_t *w = self_worker;
    w->current = NULL;
    context_restore(w->sched_rsp);
}

__attribute__((__noreturn__)) void worker_loop(void)
{
    worker_t *w = self_worker;
    for (;;) {
        if (w->after_switch) {
            void (*cb)(void *) = w->after_switch;
            void *arg = w->after_arg;
            w->after_switch = NULL;
            w->after_arg = NULL;
            cb(arg);
        }
        thread_hot_t *t = runq_pop_block(w->id);
        if (!t) {
            for (;;) pause();
        }
        w->current = t;
        if (__builtin_expect(t->rsp == NULL, 0))
            lazy_stack_alloc(t);
        context_switch(&w->sched_rsp, t->rsp);
    }
}
