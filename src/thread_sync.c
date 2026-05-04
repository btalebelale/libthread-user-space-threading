

#include "preempt.h"
#include "thread.h"
#include "thread_common.h"
#include "thread_internal.h"

#ifdef MULTICORE
#include "scheduler_mn.h"
#include <pthread.h>
#else
#include "scheduler.h"
#endif

#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

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

static inline void wq_append(thread_hot_t **head, thread_hot_t **tail, thread_hot_t *t)
{
    t->sched_next = NULL;
    if (*tail == NULL)
        *head = t;
    else
        (*tail)->sched_next = t;
    *tail = t;
}

static inline thread_hot_t *wq_pop(thread_hot_t **head, thread_hot_t **tail)
{
    thread_hot_t *t = *head;
    if (t)
    {
        *head = t->sched_next;
        if (*head == NULL)
            *tail = NULL;
    }
    return t;
}

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

    thread_hot_t *next = sched_pick_next();
    sched_remove(current);
#endif

    wq_append(&m->wait_head, &m->wait_tail, me);

#ifdef MULTICORE
    flush_last_created_pub();
    schedule_park(unlock_spinlock_cb, (void *)&m->lock);
#else
    current = next;

    if (__builtin_expect(next->rsp == NULL, 0))
        lazy_stack_alloc(next);
    context_switch(me, next);
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
    thread_hot_t *w = wq_pop(&m->wait_head, &m->wait_tail);
    if (w == NULL)
    {
        m->locked = 0;
#ifdef MULTICORE
        pthread_spin_unlock(&m->lock);
#endif
        preempt_restore(&old);
        return 0;
    }
#ifdef MULTICORE
    pthread_spin_unlock(&m->lock);
#endif

    SYNC_WAKE(w);

    preempt_restore(&old);
    return 0;
}

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
    thread_hot_t *next = sched_pick_next();
    sched_remove(current);
#endif

    wq_append(&s->wait_head, &s->wait_tail, me);

#ifdef MULTICORE
    flush_last_created_pub();
    schedule_park(unlock_spinlock_cb, (void *)&s->lock);
#else
    current = next;
    if (__builtin_expect(next->rsp == NULL, 0))
        lazy_stack_alloc(next);
    context_switch(me, next);
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
        w = wq_pop(&s->wait_head, &s->wait_tail);
#ifdef MULTICORE
    pthread_spin_unlock(&s->lock);
#endif

    if (w)
        SYNC_WAKE(w);

    preempt_restore(&old);
    return 0;
}

__attribute__((visibility("default"))) int thread_kill(thread_t target, int sig)
{
    if (sig < 1 || sig > 7)
        return -1;

    sigset_t old;
    preempt_block(&old);

    thread_hot_t *t = (thread_hot_t *)target;
    thread_cold_t *target_cold = THREAD_COLD(t);
    thread_sigset_t bit = (thread_sigset_t)(1u << sig);
    thread_hot_t *to_wake = NULL;

#ifdef MULTICORE
    pthread_spin_lock(&target_cold->cold_lock);
#endif
    target_cold->pending_sigs |= bit;
    if (target_cold->sig_waiting && (target_cold->wait_mask & bit))
    {
        target_cold->pending_sigs &= ~bit;
        target_cold->received_sig = sig;
        target_cold->sig_waiting = 0;
        target_cold->wait_mask = 0;
        to_wake = t;
    }
#ifdef MULTICORE
    pthread_spin_unlock(&target_cold->cold_lock);
#endif

    if (to_wake)
        SYNC_WAKE(to_wake);

    preempt_restore(&old);
    return 0;
}

__attribute__((visibility("default"))) int thread_sigwait(thread_sigset_t mask, int *sig)
{
    sigset_t old;
    preempt_block(&old);

    thread_cold_t *current_cold = THREAD_COLD(SYNC_ME());

#ifdef MULTICORE
    pthread_spin_lock(&current_cold->cold_lock);
#endif
    thread_sigset_t hit = current_cold->pending_sigs & mask;
    if (hit)
    {
        int s = __builtin_ctz(hit);
        current_cold->pending_sigs &= ~(thread_sigset_t)(1u << s);
#ifdef MULTICORE
        pthread_spin_unlock(&current_cold->cold_lock);
#endif
        if (sig)
            *sig = s;
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

    current_cold->wait_mask = mask;
    current_cold->sig_waiting = 1;

#ifdef MULTICORE
    flush_last_created_pub();
    schedule_park(unlock_spinlock_cb, (void *)&current_cold->cold_lock);
#else
    thread_hot_t *me = current;
    thread_hot_t *next = sched_pick_next();
    sched_remove(current);
    current = next;
    if (__builtin_expect(next->rsp == NULL, 0))
        lazy_stack_alloc(next);
    context_switch(me, next);
#endif

    if (sig)
        *sig = current_cold->received_sig;
    preempt_restore(&old);
    return 0;
}
