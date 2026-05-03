/* Constantes et macros partagées par thread.c (mono) et thread_mn.c (M:N).
 *
 * Ce header doit être inclus APRÈS thread_internal.h, qui fournit les types
 * thread_hot_t / thread_cold_t / mutex_internal_t / sem_internal_t (avec ou
 * sans champs pthread_spinlock_t selon -DMULTICORE). */
#ifndef __THREAD_COMMON_H__
#define __THREAD_COMMON_H__

#include <stddef.h>

/* ---------- Tailles d'arena ---------- */
#ifndef STACK_SIZE
#define STACK_SIZE (64 * 1024)
#endif

/* Page de garde en bas de chaque pile : SIGSEGV immédiat à l'overflow plutôt
 * que corruption silencieuse du voisin. */
#define STACK_GUARD 4096
#if STACK_SIZE <= STACK_GUARD
#error "STACK_SIZE doit être > STACK_GUARD (4096) pour la page de garde"
#endif

/* Arena de 1 GiB → max ~16k threads concurrents avec STACK_SIZE=64k. */
#define STACK_CAP   (1073741824 / STACK_SIZE)
#define THREAD_CAP  262144

/* Pile dédiée à la boucle scheduler (worker MN). Inutilisé en mono mais
 * groupé ici pour cohérence. */
#define SCHED_STACK_SIZE (64 * 1024)

/* ---------- Casts vers structures internes ---------- */
#define THREAD_COLD(hot) (&cold_slab[(hot) - hot_slab])
#define MUTEX(m)         ((mutex_internal_t *)(m))
#define SEM(s)           ((sem_internal_t  *)(s))

/* ---------- Free-list intrusive sur le bas de chaque pile ---------- */
#define STACK_FREELIST_PTR(base) \
    ((void **)((char *)(base) + STACK_SIZE - sizeof(void *)))

/* ---------- Wrappers VALGRIND ----------
 * On centralise pour que thread.c et thread_mn.c aient exactement les mêmes
 * stubs quand NVALGRIND est défini (sinon il y a duplication des #ifdef). */
#ifndef NVALGRIND
#include <valgrind/memcheck.h>
#include <valgrind/valgrind.h>
#else
#define VALGRIND_STACK_REGISTER(start, end)   (0)
#define VALGRIND_STACK_DEREGISTER(id)         ((void)(id))
#define VALGRIND_MAKE_MEM_DEFINED(addr, len)  ((void)0)
#endif

#endif /* __THREAD_COMMON_H__ */
