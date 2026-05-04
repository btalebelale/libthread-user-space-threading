#include "thread.h"
#include "scheduler.h"
#include "thread_common.h"
#include "preempt.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <signal.h>
#include <string.h>
#include <sys/time.h>
#include <errno.h>
#ifndef __x86_64__
#include <ucontext.h>
__attribute__((visibility("hidden"))) extern void portable_thread_start(void);
#endif

static void *stack_arena;
static int stack_arena_next;

static void *stack_free_head;

__attribute__((visibility("hidden"))) thread_hot_t  *hot_slab;
__attribute__((visibility("hidden"))) thread_cold_t *cold_slab;
static int thread_slab_next;

static thread_hot_t *thread_free_head;

#ifdef USE_PREEMPTION
#define PREEMPT_INTERVAL_US 20
static void preempt_handler(int sig)
{
    (void)sig;
    thread_yield();
}

__attribute__((cold)) static void preempt_init(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = preempt_handler;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGALRM, &sa, NULL);

    struct itimerval timer;
    timer.it_value.tv_sec = 0;
    timer.it_value.tv_usec = PREEMPT_INTERVAL_US;
    timer.it_interval = timer.it_value;
    setitimer(ITIMER_REAL, &timer, NULL);
}
#else
#define preempt_init() ((void)0)
#endif

__attribute__((visibility("hidden"))) thread_hot_t *current __attribute__((aligned(64))) = NULL;
__attribute__((visibility("hidden"))) thread_hot_t *last_created;
static void *(*last_created_func)(void *);
static void *last_created_arg;

__attribute__((visibility("hidden"))) int inline_executing;

#ifdef FP
#include "dedup.h"
#endif

static void *stack_alloc(void);
static void stack_release(void *s);
static void *stack_init(void *stack_base, void *(*func)(void *), void *arg, thread_hot_t *t);

__attribute__((noinline, cold, visibility("hidden"))) void flush_last_created_slow(void)
{
    thread_cold_t *lc_cold = THREAD_COLD(last_created);

lc_cold->func = last_created_func;
    lc_cold->func_arg = last_created_arg;
#ifdef FP
    dedup_insert(last_created_func, last_created_arg, last_created);
#endif
    sched_enqueue(last_created);
    last_created = NULL;
}

__attribute__((noinline, cold, visibility("hidden")))
void lazy_stack_alloc(thread_hot_t *t)
{
    thread_cold_t *target_cold = THREAD_COLD(t);
    void *stack = stack_alloc();
    if (__builtin_expect(!stack, 0))
    {

        fprintf(stderr,
                "libthread: stack arena exhausted (max %d threads concurrents)\n",
                STACK_CAP);
        abort();
    }
    target_cold->stack_base = stack;
    t->rsp = stack_init(stack, target_cold->func, target_cold->func_arg, t);
#ifndef NVALGRIND

    target_cold->valgrind_stackid = VALGRIND_STACK_REGISTER(
        (char *)stack + STACK_GUARD, (char *)stack + STACK_SIZE);
#endif
}

static inline __attribute__((always_inline)) void flush_last_created(void)
{
    if (__builtin_expect(last_created != NULL, 0))
        flush_last_created_slow();
}

__attribute__((cold)) static void mem_init(void)
{

    stack_arena = mmap(NULL, (size_t)STACK_CAP * STACK_SIZE, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (__builtin_expect(stack_arena == MAP_FAILED, 0))
    {
        perror("mmap stack arena");
        exit(1);
    }

    madvise(stack_arena, (size_t)STACK_CAP * STACK_SIZE, MADV_HUGEPAGE);

    hot_slab = mmap(NULL, (size_t)THREAD_CAP * sizeof(thread_hot_t), PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (__builtin_expect(hot_slab == MAP_FAILED, 0))
    {
        perror("mmap hot slab");
        exit(1);
    }

    cold_slab = mmap(NULL, (size_t)THREAD_CAP * sizeof(thread_cold_t), PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (__builtin_expect(cold_slab == MAP_FAILED, 0))
    {
        perror("mmap cold slab");
        exit(1);
    }

#ifdef FP
    dedup_mem_init();
#endif

    stack_arena_next = 0;
    stack_free_head = NULL;
    thread_slab_next = 0;
    thread_free_head = NULL;
}

__attribute__((cold)) static void mem_destroy(void)
{
    if (stack_arena && stack_arena != MAP_FAILED)
        munmap(stack_arena, (size_t)STACK_CAP * STACK_SIZE);
    if (hot_slab && hot_slab != MAP_FAILED)
        munmap(hot_slab, (size_t)THREAD_CAP * sizeof(thread_hot_t));
    if (cold_slab && cold_slab != MAP_FAILED)
        munmap(cold_slab, (size_t)THREAD_CAP * sizeof(thread_cold_t));
#ifdef FP
    dedup_mem_destroy();
#endif
    stack_arena = NULL;
    hot_slab = NULL;
    cold_slab = NULL;
}

static void *stack_alloc(void)
{
    if (__builtin_expect(stack_free_head != NULL, 1))
    {
        void *s = stack_free_head;
        stack_free_head = *STACK_FREELIST_PTR(s);
        return s;
    }
    if (__builtin_expect(stack_arena_next >= STACK_CAP, 0))
        return NULL;

    void *s = (char *)stack_arena + ((size_t)stack_arena_next++ * STACK_SIZE);

    return s;
}

static void stack_release(void *s)
{
    *STACK_FREELIST_PTR(s) = stack_free_head;
    stack_free_head = s;
}

static thread_hot_t *thread_alloc(void)
{
    if (__builtin_expect(thread_free_head != NULL, 1))
    {
        thread_hot_t *t = thread_free_head;
        thread_free_head = (thread_hot_t *)t->rsp;
        return t;
    }
    if (__builtin_expect(thread_slab_next >= THREAD_CAP, 0))
        return NULL;
    return &hot_slab[thread_slab_next++];
}

static void thread_release(thread_hot_t *t)
{
    t->rsp = (void *)thread_free_head;
    thread_free_head = t;
}

static char exit_stack[4096] __attribute__((aligned(16)));

static thread_hot_t exit_thread;

__attribute__((visibility("hidden"))) void thread_entry(void *(*func)(void *), void *arg)
{
    THREAD_COLD(current)->started = 1;
    preempt_unblock_self();
    void *ret = func(arg);
    thread_exit(ret);
    __builtin_unreachable();
}

static int stack_color_counter;

static void *stack_init(void *stack_base, void *(*func)(void *), void *arg, thread_hot_t *t)
{
#ifdef __x86_64__
    uintptr_t sp = (uintptr_t)stack_base + STACK_SIZE;

    sp -= ((unsigned)stack_color_counter++ & 0xFF) * 64;

    sp &= ~(uintptr_t)0xF;

    void **s = (void **)sp;

*(--s) = (void *)thread_trampoline;

t->saved_rbx = NULL;
    t->saved_rbp = NULL;
    t->saved_r12 = (void *)func;
    t->saved_r13 = arg;
    t->saved_r14 = NULL;
    t->saved_r15 = NULL;

    VALGRIND_MAKE_MEM_DEFINED(s, sizeof(void *));

    return s;
#else

    (void)func; (void)arg; (void)stack_color_counter;
    ucontext_t *uc = (ucontext_t *)((char *)stack_base + STACK_GUARD);
    char  *ss_sp   = (char *)uc + sizeof(ucontext_t);
    size_t ss_size = STACK_SIZE - STACK_GUARD - sizeof(ucontext_t);
    getcontext(uc);
    uc->uc_link          = NULL;
    uc->uc_stack.ss_sp   = ss_sp;
    uc->uc_stack.ss_size = ss_size;
    makecontext(uc, portable_thread_start, 0);
    return uc;
#endif
}

__attribute__((cold)) static void clean_exit(void)
{
    thread_cold_t *current_cold = THREAD_COLD(current);
    if (current_cold->stack_base)
    {
#ifndef NVALGRIND
        VALGRIND_STACK_DEREGISTER(current_cold->valgrind_stackid);
#endif
    }
    current = NULL;
    mem_destroy();
    exit(0);
}

__attribute__((cold)) static void thread_system_destroy(void)
{
#ifdef USE_PREEMPTION
    struct itimerval off;
    memset(&off, 0, sizeof(off));
    setitimer(ITIMER_REAL, &off, NULL);
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGALRM);
    sigprocmask(SIG_BLOCK, &mask, NULL);
#endif

    if (current != NULL)
    {
        while (!is_sched_empty())
        {
            thread_hot_t *t = sched_dequeue_fifo();
            thread_cold_t *target_cold = THREAD_COLD(t);
            if (target_cold->stack_base)
            {
#ifndef NVALGRIND
                VALGRIND_STACK_DEREGISTER(target_cold->valgrind_stackid);
#endif
            }
        }
    }
    current = NULL;
    mem_destroy();
}

__attribute__((constructor, cold)) static void init_system(void)
{
    if (__builtin_expect(current != NULL, 0))
        return;
    mem_init();
    last_created = NULL;
    atexit(thread_system_destroy);

    current = thread_alloc();
    if (__builtin_expect(!current, 0))
    {
        perror("thread_alloc main");
        exit(1);
    }
#ifdef __x86_64__
    current->rsp = NULL;
#else

    static ucontext_t main_uc;
    current->rsp = &main_uc;
#endif
    current->sched_next = current;

    thread_cold_t *current_cold = THREAD_COLD(current);
    thread_cold_reset(current_cold);
    current_cold->sched_prev = current;
    current_cold->priority = THREAD_PRIORITY_DEFAULT;
    current_cold->started = 1;

#ifdef __x86_64__

    uintptr_t sp = (uintptr_t)(exit_stack + sizeof(exit_stack));
    sp &= ~(uintptr_t)0xF;
    void **s = (void **)sp;
    *(--s) = (void *)clean_exit;
    exit_thread.rsp = s;
    VALGRIND_MAKE_MEM_DEFINED(s, sizeof(void *));
#else
    static ucontext_t exit_uc;
    getcontext(&exit_uc);
    exit_uc.uc_link          = NULL;
    exit_uc.uc_stack.ss_sp   = exit_stack;
    exit_uc.uc_stack.ss_size = sizeof(exit_stack);
    makecontext(&exit_uc, clean_exit, 0);
    exit_thread.rsp = &exit_uc;
#endif

    preempt_init();
}

__attribute__((visibility("default"))) thread_t thread_self(void)
{
    return (thread_t)current;
}

__attribute__((visibility("default"))) int thread_create(thread_t *newthread, void *(*func)(void *),
                                                         void *arg)
{
    sigset_t old;
    preempt_block(&old);

#ifdef FP
    if (__builtin_expect(last_created != NULL, 0))
    {
        if (last_created_func == func && last_created_arg == arg)
        {
            THREAD_COLD(last_created)->refcount++;
            *newthread = (thread_t)last_created;
            preempt_restore(&old);
            return 0;
        }
    }
    thread_hot_t *existing = dedup_lookup(func, arg);
    if (__builtin_expect(existing != NULL, 0))
    {
        THREAD_COLD(existing)->refcount++;
        *newthread = (thread_t)existing;
        preempt_restore(&old);
        return 0;
    }
#endif

    thread_hot_t *t = thread_alloc();
    if (__builtin_expect(!t, 0))
        return -1;

    thread_cold_t *target_cold = THREAD_COLD(t);
    thread_cold_reset(target_cold);
    t->rsp = NULL;
    target_cold->priority = THREAD_PRIORITY_DEFAULT;

    flush_last_created();
    last_created = t;
    last_created_func = func;
    last_created_arg = arg;
    *newthread = (thread_t)t;

    preempt_restore(&old);

    return 0;
}

__attribute__((visibility("default"))) int thread_setpriority(thread_t thread, int priority)
{
    if (priority < THREAD_PRIORITY_MIN || priority > THREAD_PRIORITY_MAX)
        return -1;
    thread_hot_t *t = (thread_hot_t *)thread;
    THREAD_COLD(t)->priority = priority;
    return 0;
}

__attribute__((visibility("default"))) int thread_getpriority(thread_t thread, int *priority)
{
    if (!priority)
        return -1;
    thread_hot_t *t = (thread_hot_t *)thread;
    *priority = THREAD_COLD(t)->priority;
    return 0;
}

#if defined(USE_PRIORITY)

__attribute__((visibility("hidden"))) extern int thread_yield_asm(void);

__attribute__((visibility("default"))) int thread_yield(void)
{
    if (__builtin_expect(inline_executing > 0, 0))
        return 0;

    sigset_t old;
    preempt_block(&old);
    flush_last_created();

    if (is_sched_empty())
    {
        preempt_restore(&old);
        return 0;
    }
    thread_hot_t *best = sched_pick_highest();

    if (THREAD_COLD(best)->priority < THREAD_COLD(current)->priority)
    {
        preempt_restore(&old);
        return 0;
    }

    thread_hot_t *prev = current;
    current = best;
    if (__builtin_expect(best->rsp == NULL, 0))
        lazy_stack_alloc(best);

    context_switch(prev, best);
    preempt_restore(&old);
    return 0;
}

#elif defined(USE_PREEMPTION)

__attribute__((visibility("hidden"))) extern int thread_yield_asm(void);

__attribute__((visibility("default"))) int thread_yield(void)
{
    if (__builtin_expect(inline_executing > 0, 0))
        return 0;
    sigset_t old;
    preempt_block(&old);
    int r = thread_yield_asm();
    preempt_restore(&old);
    return r;
}

#endif

thread_cold_t *uf_find(thread_cold_t *t){
    while(t->parent != t){
        t = t->parent = t->parent->parent;
    }
    return t;
}

int uf_union(thread_cold_t *a, thread_cold_t *b){
    a = uf_find(a);
    b = uf_find(b);

    if(a == b) return -1;

    while(a->rank < b->rank){
        thread_cold_t *tmp = a;
        a = b;
        b = tmp;
    }

    b->parent = a;
    if(a->rank == b->rank) a->rank++;
    return 0;
}

__attribute__((noinline))
static void inline_run(thread_hot_t *t, void *(*fn)(void *), void *ar)
{
    thread_cold_t *target_cold = THREAD_COLD(t);
    thread_hot_t *prev = current;
    current = t;
    sched_replace(prev, t);

    target_cold->started = 1;
    inline_executing++;
    jmp_buf jb;
    if (__builtin_expect(_setjmp(jb) == 0, 1))
    {
        target_cold->inline_jmpbuf = &jb;
        target_cold->retval = fn(ar);
    }
    target_cold->inline_jmpbuf = NULL;
    target_cold->state = FINISHED;
    inline_executing--;

    sched_replace(t, prev);
    current = prev;
}

static inline __attribute__((always_inline))
void join_fast_inline(thread_hot_t *t)
{
    last_created = NULL;
    if (__builtin_expect(t->rsp == NULL, 1))
        inline_run(t, last_created_func, last_created_arg);
}

static inline __attribute__((always_inline))
void join_generalized_inline(thread_hot_t *t, thread_cold_t *target_cold)
{
    flush_last_created();
    sched_remove(t);

    if (target_cold->stack_base)
    {
#ifndef NVALGRIND
        VALGRIND_STACK_DEREGISTER(target_cold->valgrind_stackid);
#endif
        stack_release(target_cold->stack_base);
        target_cold->stack_base = NULL;
    }
    t->rsp = NULL;

    inline_run(t, target_cold->func, target_cold->func_arg);
}

static inline __attribute__((always_inline))
int join_park(thread_cold_t *target_cold)
{
    target_cold->waiting = current;
    flush_last_created();

    if (__builtin_expect(is_sched_empty(), 0))
        return -1;

    thread_hot_t *next = sched_pick_next();
    sched_remove(current);

    thread_hot_t *prev = current;
    current = next;

    if (__builtin_expect(next->rsp == NULL, 0))
        lazy_stack_alloc(next);

    context_switch(prev, next);
    return 0;
}

static inline __attribute__((always_inline))
void join_cleanup(thread_hot_t *t, thread_cold_t *target_cold, void **retval)
{
    if (retval)
        *retval = target_cold->retval;

    target_cold->refcount--;
    if (__builtin_expect(target_cold->refcount == 0, 1))
    {
        if (target_cold->func != NULL)
        {
#ifdef FP
            dedup_remove(target_cold->func, target_cold->func_arg);
#endif
            target_cold->func = NULL;
        }

        if (target_cold->stack_base)
        {
#ifndef NVALGRIND
            VALGRIND_STACK_DEREGISTER(target_cold->valgrind_stackid);
#endif
            stack_release(target_cold->stack_base);
        }

        thread_release(t);
    }
}

__attribute__((visibility("default"), hot)) int thread_join(thread_t thread, void **retval)
{
    sigset_t old;
    preempt_block(&old);

    thread_hot_t *t = (thread_hot_t *)thread;
    thread_cold_t *target_cold = THREAD_COLD(t);

    if (__builtin_expect(target_cold->state != FINISHED, 1))
    {
        if (uf_union(THREAD_COLD(current), target_cold) == -1)
        {
            preempt_restore(&old);
            return EDEADLK;
        }

        if (__builtin_expect(last_created == t, 1))
            join_fast_inline(t);
        else if (__builtin_expect(!target_cold->started && target_cold->func != NULL, 0))
            join_generalized_inline(t, target_cold);
        else if (join_park(target_cold) == -1)
        {
            preempt_restore(&old);
            return -1;
        }
    }

    join_cleanup(t, target_cold, retval);
    preempt_restore(&old);
    return 0;
}

__attribute__((visibility("default"), hot)) void thread_exit(void *retval)
{
    sigset_t old;
    preempt_block(&old);

    thread_cold_t *current_cold = THREAD_COLD(current);
    current_cold->retval = retval;

    if (__builtin_expect(current_cold->inline_jmpbuf != NULL, 0))
    {
        _longjmp(*current_cold->inline_jmpbuf, 1);
        __builtin_unreachable();
    }

    current_cold->state = FINISHED;

    if (__builtin_expect(current_cold->waiting != NULL, 1))
    {
        thread_hot_t *w = current_cold->waiting;
        sched_replace(current, w);
        current = w;
        if (__builtin_expect(w->rsp == NULL, 0))
            lazy_stack_alloc(w);
        context_restore(w);
    }

    if (__builtin_expect(current_cold->func != NULL, 1))
    {
#ifdef FP
        dedup_remove(current_cold->func, current_cold->func_arg);
#endif
        current_cold->func = NULL;
    }

    flush_last_created();

    if (__builtin_expect(is_sched_empty(), 0))
        context_restore(&exit_thread);

    thread_hot_t *next = sched_pick_next();

    sched_remove(current);

void *exit_stack = current_cold->stack_base;
    current_cold->stack_base = NULL;

    current = next;

    if (__builtin_expect(next->rsp == NULL, 0))
        lazy_stack_alloc(next);

    if (__builtin_expect(exit_stack != NULL, 1))
    {
        *STACK_FREELIST_PTR(exit_stack) = stack_free_head;
        stack_free_head = exit_stack;
    }

    context_restore(next);
}

__attribute__((visibility("hidden"))) void flush_last_created_pub(void)
{
    flush_last_created();
}
