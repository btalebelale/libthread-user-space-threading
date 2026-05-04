/* context_switch_portable.c — fallback ucontext pour archis non-x86_64.
 *
 * Linux + GCC restent acquis ; seule l'absence d'asm x86_64 nous force ce
 * détour par <ucontext.h>. Aucune ambition de perf : objectif strict =
 * "ça compile et ça tourne" sur ARM, RISC-V, etc.
 *
 * Convention sur ces archis : t->rsp pointe vers un ucontext_t (au lieu
 * d'une vraie valeur RSP). Les sites d'usage (context_switch / restore /
 * thread_yield_asm) traitent le pointeur de manière opaque, donc le reste
 * du code n'a rien à savoir.
 *
 * Limitation assumée : MULTICORE non supporté ici (le mode MN tape sur
 * `self_worker->current` et tout un échafaudage pthread x86-centré). */

#if !defined(__x86_64__) && !defined(MULTICORE)

#include <ucontext.h>
#include "thread_internal.h"
#include "thread_common.h"  /* THREAD_COLD */

extern thread_hot_t *last_created;
extern int inline_executing;
extern void flush_last_created_slow(void);

/* Trampoline appelé par makecontext : à ce moment `current` désigne déjà
 * le nouveau thread (le swap a basculé sur sa pile), donc on lit func/arg
 * depuis son cold. */
extern void thread_entry(void *(*func)(void *), void *arg);

__attribute__((visibility("hidden"))) void portable_thread_start(void)
{
    thread_cold_t *cc = THREAD_COLD(current);
    thread_entry(cc->func, cc->func_arg);
}

__attribute__((visibility("hidden")))
void context_switch(void **old_rsp, void *new_rsp)
{
    swapcontext((ucontext_t *)*old_rsp, (ucontext_t *)new_rsp);
}

__attribute__((visibility("hidden"), __noreturn__))
void context_restore(void *new_rsp)
{
    setcontext((ucontext_t *)new_rsp);
    __builtin_unreachable();
}

/* Jamais référencé sur ce path (stack_init utilise makecontext au lieu de
 * pousser une adresse de retour), mais le symbole reste exporté pour ne
 * pas casser un éventuel `extern` ailleurs. */
__attribute__((visibility("hidden"))) void thread_trampoline(void) {}

/* Équivalent C de l'asm thread_yield_asm — même logique exacte. */
__attribute__((visibility("hidden")))
int thread_yield_asm(void)
{
    if (__builtin_expect(inline_executing > 0, 0))
        return 0;
    if (__builtin_expect(last_created != NULL, 0))
        flush_last_created_slow();

    thread_hot_t *prev = current;
    thread_hot_t *next = prev->sched_next;
    if (next == prev)
        return 0;

    if (__builtin_expect(next->rsp == NULL, 0))
        lazy_stack_alloc(next);

    current = next;
    swapcontext((ucontext_t *)prev->rsp, (ucontext_t *)next->rsp);
    return 0;
}

/* Sans USE_PREEMPTION/USE_PRIORITY l'asm aliasait thread_yield sur
 * thread_yield_asm. On reproduit l'alias ici. */
#if !defined(USE_PREEMPTION) && !defined(USE_PRIORITY)
__attribute__((visibility("default")))
int thread_yield(void) { return thread_yield_asm(); }
#endif

#endif /* !__x86_64__ && !MULTICORE */
