
#ifndef __THREAD_COMMON_H__
#define __THREAD_COMMON_H__

#include <stddef.h>

#ifndef STACK_SIZE

#define STACK_SIZE (24 * 1024)
#endif

#define STACK_GUARD 256
#if STACK_SIZE <= STACK_GUARD
#error "STACK_SIZE doit être > STACK_GUARD pour la page de garde"
#endif

#define THREAD_CAP  20000
#define STACK_CAP   THREAD_CAP

#define SCHED_STACK_SIZE (64 * 1024)

#define THREAD_COLD(hot) (&cold_slab[(hot) - hot_slab])
#define MUTEX(m)         ((mutex_internal_t *)(m))
#define SEM(s)           ((sem_internal_t  *)(s))

#define STACK_FREELIST_PTR(base) \
    ((void **)((char *)(base) + STACK_SIZE - sizeof(void *)))

#ifndef NVALGRIND
#include <valgrind/memcheck.h>
#include <valgrind/valgrind.h>
#else
#define VALGRIND_STACK_REGISTER(start, end)   (0)
#define VALGRIND_STACK_DEREGISTER(id)         ((void)(id))
#define VALGRIND_MAKE_MEM_DEFINED(addr, len)  ((void)0)
#endif

#endif
