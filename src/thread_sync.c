/* thread_sync.c — mutex, sémaphores, signaux thread.
 *
 * Compilé une fois en mono (sans MULTICORE) et une fois en MN (avec) ; le
 * hot path scheduler/yield/create/join/exit reste dans thread.c (mono) et
 * thread_mn.c (MN), trop divergent pour être mutualisé proprement.
 *
 * Différences mono / MN encapsulées localement :
 *   - locking interne d'un mutex/sem/cold (no-op en mono single-threaded)
 *   - identification du thread courant (`current` global vs `self_worker->current`)
 *   - réveil d'un thread (sched_enqueue vs runq_push)
 *   - park sur un objet : mono fait sched_remove + context_switch direct,
 *     MN délègue à schedule_park avec callback de release du lock. */

#include "thread.h"
#include "thread_internal.h"
#include "thread_common.h"
#include "preempt.h"

#ifdef MULTICORE
#include "scheduler_mn.h"
#include <pthread.h>
#else
#include "scheduler.h"
#endif

#include <stdint.h>
#include <signal.h>
#include <errno.h>
#include <stddef.h>

/* Wrapper sur flush_last_created (static inline local à thread.c / thread_mn.c).
 * Exposé hidden pour que thread_sync.c puisse l'appeler sans dupliquer la
 * logique de flush, qui dépend de variables propres à chaque mode. */
__attribute__((visibility("hidden"))) extern void flush_last_created_pub(void);

#ifdef MULTICORE
#define SYNC_ME() (self_worker->current)
#define SYNC_WAKE(t) runq_push(t)

static void unlock_spinlock_cb(void *arg)
{
    pthread_spin_unlock((pthread_spinlock_t *)arg);
}
#else
#define SYNC_ME() (current)
#define SYNC_WAKE(t) sched_enqueue(t)
#endif


/* ---------- Mutex ---------- */
__attribute__((visibility("default"))) int thread_mutex_init(thread_mutex_t *mutex)
{
    mutex_internal_t *m = MUTEX(mutex);
    m->locked = 0;
    m->wait_head = NULL;
    m->wait_tail = NULL;
#ifdef MULTICORE
    pthread_spin_init(&m->lock, PTHREAD_PROCESS_PRIVATE);
#endif
    return 0;
}

__attribute__((visibility("default"))) int thread_mutex_destroy(thread_mutex_t *mutex)
{
#ifdef MULTICORE
    mutex_internal_t *m = MUTEX(mutex);
    pthread_spin_destroy(&m->lock);
#else
    (void)mutex;
#endif
    return 0;
}

__attribute__((visibility("default"), hot)) int thread_mutex_lock(thread_mutex_t *mutex)
{
    sigset_t old;
    preempt_block(&old);
    mutex_internal_t *m = MUTEX(mutex);

#ifdef MULTICORE
    pthread_spin_lock(&m->lock);
#endif
    if (__builtin_expect(!m->locked, 1))
    {
        m->locked = 1;
#ifdef MULTICORE
        pthread_spin_unlock(&m->lock);
#endif
        preempt_restore(&old);
        return 0;
    }

#ifndef MULTICORE
    if (__builtin_expect(is_sched_empty(), 0))
    {
        preempt_restore(&old);
        return -1;
    }
#endif

    thread_hot_t *me = SYNC_ME();
#ifndef MULTICORE
    /* IMPORTANT : retirer current de la liste circulaire AVANT de
     * surcharger me->sched_next. Sinon sched_remove suit un sched_next
     * NULL et crash. (En MN, schedule_park sauve me->rsp et n'utilise
     * pas sched_*, donc l'ordre est libre.) */
    thread_hot_t *next = current->sched_prev;
    sched_remove(current);
#endif

    me->sched_next = NULL;
    if (m->wait_tail == NULL) m->wait_head = me;
    else m->wait_tail->sched_next = me;
    m->wait_tail = me;

#ifdef MULTICORE
    flush_last_created_pub();
    schedule_park(unlock_spinlock_cb, (void *)&m->lock);
#else
    current = next;
    __builtin_prefetch(next->rsp, 0, 3);
    context_switch(&me->rsp, next->rsp);
#endif

    preempt_restore(&old);
    return 0;
}

__attribute__((visibility("default"), hot)) int thread_mutex_unlock(thread_mutex_t *mutex)
{
    sigset_t old;
    preempt_block(&old);
    mutex_internal_t *m = MUTEX(mutex);

#ifdef MULTICORE
    pthread_spin_lock(&m->lock);
#endif
    thread_hot_t *w = m->wait_head;
    if (w == NULL)
    {
        m->locked = 0;
#ifdef MULTICORE
        pthread_spin_unlock(&m->lock);
#endif
        preempt_restore(&old);
        return 0;
    }
    m->wait_head = w->sched_next;
    if (m->wait_head == NULL) m->wait_tail = NULL;
#ifdef MULTICORE
    pthread_spin_unlock(&m->lock);
#endif
    /* Le mutex reste verrouillé : ownership transmis au thread réveillé. */
    SYNC_WAKE(w);

    preempt_restore(&old);
    return 0;
}


/* ---------- Sémaphore ---------- */
__attribute__((visibility("default"))) int thread_sem_init(thread_sem_t *sem, int value)
{
    sem_internal_t *s = SEM(sem);
    s->count = value;
    s->wait_head = NULL;
    s->wait_tail = NULL;
#ifdef MULTICORE
    pthread_spin_init(&s->lock, PTHREAD_PROCESS_PRIVATE);
#endif
    return 0;
}

__attribute__((visibility("default"))) int thread_sem_destroy(thread_sem_t *sem)
{
#ifdef MULTICORE
    sem_internal_t *s = SEM(sem);
    pthread_spin_destroy(&s->lock);
#else
    (void)sem;
#endif
    return 0;
}

__attribute__((visibility("default"), hot)) int thread_sem_wait(thread_sem_t *sem)
{
    sigset_t old;
    preempt_block(&old);
    sem_internal_t *s = SEM(sem);

#ifdef MULTICORE
    pthread_spin_lock(&s->lock);
#endif
    s->count--;
    if (s->count >= 0)
    {
#ifdef MULTICORE
        pthread_spin_unlock(&s->lock);
#endif
        preempt_restore(&old);
        return 0;
    }

#ifndef MULTICORE
    if (__builtin_expect(is_sched_empty(), 0))
    {
        preempt_restore(&old);
        return -1;
    }
#endif

    thread_hot_t *me = SYNC_ME();
#ifndef MULTICORE
    thread_hot_t *next = current->sched_prev;
    sched_remove(current);
#endif

    me->sched_next = NULL;
    if (s->wait_tail == NULL) s->wait_head = me;
    else s->wait_tail->sched_next = me;
    s->wait_tail = me;

#ifdef MULTICORE
    flush_last_created_pub();
    schedule_park(unlock_spinlock_cb, (void *)&s->lock);
#else
    current = next;
    if (__builtin_expect(next->rsp == NULL, 0))
        lazy_stack_alloc(next);
    context_switch(&me->rsp, next->rsp);
#endif

    preempt_restore(&old);
    return 0;
}

__attribute__((visibility("default"), hot)) int thread_sem_post(thread_sem_t *sem)
{
    sigset_t old;
    preempt_block(&old);
    sem_internal_t *s = SEM(sem);

#ifdef MULTICORE
    pthread_spin_lock(&s->lock);
#endif
    s->count++;
    thread_hot_t *w = NULL;
    if (s->count <= 0)
    {
        w = s->wait_head;
        s->wait_head = w->sched_next;
        if (s->wait_head == NULL) s->wait_tail = NULL;
    }
#ifdef MULTICORE
    pthread_spin_unlock(&s->lock);
#endif

    if (w) SYNC_WAKE(w);

    preempt_restore(&old);
    return 0;
}


/* ---------- Signaux libthread ---------- */
__attribute__((visibility("default"))) int thread_kill(thread_t target, int sig)
{
    if (sig < 1 || sig > 7) return -1;

    sigset_t old;
    preempt_block(&old);

    thread_hot_t  *t  = (thread_hot_t *)target;
    thread_cold_t *tc = THREAD_COLD(t);
    thread_sigset_t bit = (thread_sigset_t)(1u << sig);
    thread_hot_t *to_wake = NULL;

#ifdef MULTICORE
    pthread_spin_lock(&tc->cold_lock);
#endif
    tc->pending_sigs |= bit;
    if (tc->sig_waiting && (tc->wait_mask & bit))
    {
        tc->pending_sigs &= ~bit;
        tc->received_sig = sig;
        tc->sig_waiting  = 0;
        tc->wait_mask    = 0;
        to_wake = t;
    }
#ifdef MULTICORE
    pthread_spin_unlock(&tc->cold_lock);
#endif

    if (to_wake) SYNC_WAKE(to_wake);

    preempt_restore(&old);
    return 0;
}

__attribute__((visibility("default"))) int thread_sigwait(thread_sigset_t mask, int *sig)
{
    sigset_t old;
    preempt_block(&old);

    thread_cold_t *cc = THREAD_COLD(SYNC_ME());

#ifdef MULTICORE
    pthread_spin_lock(&cc->cold_lock);
#endif
    thread_sigset_t hit = cc->pending_sigs & mask;
    if (hit)
    {
        int s = __builtin_ctz(hit);
        cc->pending_sigs &= ~(thread_sigset_t)(1u << s);
#ifdef MULTICORE
        pthread_spin_unlock(&cc->cold_lock);
#endif
        if (sig) *sig = s;
        preempt_restore(&old);
        return 0;
    }

#ifndef MULTICORE
    if (__builtin_expect(is_sched_empty(), 0))
    {
        preempt_restore(&old);
        return -1;
    }
#endif

    cc->wait_mask   = mask;
    cc->sig_waiting = 1;

#ifdef MULTICORE
    flush_last_created_pub();
    schedule_park(unlock_spinlock_cb, (void *)&cc->cold_lock);
#else
    thread_hot_t *me = current;
    thread_hot_t *next = current->sched_prev;
    sched_remove(current);
    current = next;
    if (__builtin_expect(next->rsp == NULL, 0))
        lazy_stack_alloc(next);
    context_switch(&me->rsp, next->rsp);
#endif

    /* Au réveil par thread_kill, cc->received_sig est défini. */
    if (sig) *sig = cc->received_sig;
    preempt_restore(&old);
    return 0;
}
