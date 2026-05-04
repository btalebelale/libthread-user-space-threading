

#if !defined(__x86_64__) && !defined(MULTICORE)

#include "thread_common.h"
#include "thread_internal.h"
#include <ucontext.h>

extern thread_hot_t *last_created;
extern int inline_executing;
extern void flush_last_created_slow(void);

extern void thread_entry(void *(*func)(void *), void *arg);

__attribute__((visibility("hidden"))) void portable_thread_start(void)
{
    thread_cold_t *cc = THREAD_COLD(current);
    thread_entry(cc->func, cc->func_arg);
}

__attribute__((visibility("hidden"))) void context_switch(void **old_rsp, void *new_rsp)
{
    swapcontext((ucontext_t *)*old_rsp, (ucontext_t *)new_rsp);
}

__attribute__((visibility("hidden"), __noreturn__)) void context_restore(void *new_rsp)
{
    setcontext((ucontext_t *)new_rsp);
    __builtin_unreachable();
}

__attribute__((visibility("hidden"))) void thread_trampoline(void)
{
}

__attribute__((visibility("hidden"))) int thread_yield_asm(void)
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

#if !defined(USE_PREEMPTION) && !defined(USE_PRIORITY)
__attribute__((visibility("default"))) int thread_yield(void)
{
    return thread_yield_asm();
}
#endif

#endif
