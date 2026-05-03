/* Helpers de blocage/restauration de SIGALRM, partagés par thread.c, thread_mn.c
 * et thread_sync.c. Variantes :
 *   - mono USE_PREEMPTION       : sigprocmask (single-threaded)
 *   - MN sans LIBTHREAD_MN_PREEMPT : no-op (la sched_stack n'aime pas être
 *                                   préemptée — voir commentaire thread_mn.c)
 *   - MN avec LIBTHREAD_MN_PREEMPT : pthread_sigmask (multi-thread)
 *   - sans USE_PREEMPTION       : no-op
 */
#ifndef __PREEMPT_H__
#define __PREEMPT_H__

#include <signal.h>
#include <string.h>

#if defined(USE_PREEMPTION) && (!defined(MULTICORE) || defined(LIBTHREAD_MN_PREEMPT))

#ifdef MULTICORE
#include <pthread.h>
#define PREEMPT_SIGMASK(how, set, old) pthread_sigmask((how), (set), (old))
#else
#define PREEMPT_SIGMASK(how, set, old) sigprocmask((how), (set), (old))
#endif

static inline void preempt_block(sigset_t *old)
{
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGALRM);
    PREEMPT_SIGMASK(SIG_BLOCK, &mask, old);
}

static inline void preempt_restore(sigset_t *old)
{
    PREEMPT_SIGMASK(SIG_SETMASK, old, NULL);
}

static inline void preempt_unblock_self(void)
{
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGALRM);
    PREEMPT_SIGMASK(SIG_UNBLOCK, &mask, NULL);
}

#define PREEMPT_ENABLED 1

#else

#define preempt_block(old)        ((void)(old))
#define preempt_restore(old)      ((void)(old))
#define preempt_unblock_self()    ((void)0)

#endif

#endif
