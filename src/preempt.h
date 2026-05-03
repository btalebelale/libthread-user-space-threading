/* Helpers de blocage/restauration de SIGALRM, partagés par thread.c, thread_mn.c
 * et thread_sync.c. Variantes :
 *   - mono USE_PREEMPTION       : sigprocmask (single-threaded)
 *   - MN avec LIBTHREAD_MN_PREEMPT (défaut quand MULTICORE=1) : pthread_sigmask.
 *     Invariant : SIGALRM masqué sur la sched_stack, démasqué sur la
 *     user_stack. Voir commentaire détaillé en tête du bloc Preemption
 *     dans thread_mn.c.
 *   - MN avec LIBTHREAD_MN_NO_PREEMPT : no-op (opt-out explicite)
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
