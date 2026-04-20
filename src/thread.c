#pragma GCC optimize("Ofast,unroll-loops")

#include "thread.h"
#include "scheduler.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <signal.h>
#include <string.h>                                                                                                            
#include <sys/time.h>
#include <errno.h>


#ifndef NVALGRIND
#include <valgrind/memcheck.h>
#include <valgrind/valgrind.h>
#else
#define VALGRIND_STACK_REGISTER(start, end) (0)
#define VALGRIND_STACK_DEREGISTER(id) ((void)(id))
#define VALGRIND_MAKE_MEM_DEFINED(addr, len) ((void)0)
#endif

#ifndef STACK_SIZE
#define STACK_SIZE (64 * 1024)
#endif

#define STACK_CAP (1073741824 / STACK_SIZE)

#define THREAD_CAP 262144

static void *stack_arena;
static int stack_arena_next;

static void *stack_free_head;

static thread_hot_t *hot_slab;
static thread_cold_t *cold_slab;
static int thread_slab_next;

static thread_hot_t *thread_free_head;


static int dedup_enabled = 1;


// La préemption
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
                                                                                                                                      
static inline void preempt_block(sigset_t *old)                                                                                       
{
    sigset_t mask;                                                                                                                    
    sigemptyset(&mask);
    sigaddset(&mask, SIGALRM);                                                                                                        
    sigprocmask(SIG_BLOCK, &mask, old);
}                                                                                                                                     
                
static inline void preempt_restore(sigset_t *old)                                                                                     
{
    sigprocmask(SIG_SETMASK, old, NULL);                                                                                              
}               
#else
#define preempt_init()            ((void)0)
#define preempt_block(old)        ((void)(old))                                                                                       
#define preempt_restore(old)      ((void)(old))

#endif 


__attribute__((visibility("hidden"))) thread_hot_t *last_created;
static void *(*last_created_func)(void *);
static void *last_created_arg;

__attribute__((visibility("hidden"))) int inline_executing;

#include "dedup.h"

#define THREAD_COLD(hot) (&cold_slab[(hot) - hot_slab])

static void *stack_alloc(void);
static void stack_release(void *s);
static void *stack_init(void *stack_base, void *(*func)(void *), void *arg);

__attribute__((noinline, cold, visibility("hidden"))) void flush_last_created_slow(void)
{
    thread_cold_t *lc = THREAD_COLD(last_created);

    // Deferred stack allocation: don't allocate stack here.
    // Stack will be allocated lazily in yield if thread is actually scheduled,
    // or skipped entirely if the thread is inline-joined.
    lc->func = last_created_func;
    lc->func_arg = last_created_arg;
    if(dedup_enabled){
        dedup_insert(last_created_func, last_created_arg, last_created);
    }
    sched_enqueue(last_created);
    last_created = NULL;
}

// Lazy stack allocation: called from yield when next thread has no stack
__attribute__((noinline, cold, visibility("hidden")))
void lazy_stack_alloc(thread_hot_t *t)
{
    thread_cold_t *tc = THREAD_COLD(t);
    void *stack = stack_alloc();
    if (__builtin_expect(!stack, 0))
    {
        // No stacks left — run the function inline as fallback
        tc->retval = tc->func(tc->func_arg);
        tc->state = FINISHED;
        return;
    }
    tc->stack_base = stack;
    t->rsp = stack_init(stack, tc->func, tc->func_arg);
#ifndef NVALGRIND
    tc->valgrind_stackid = VALGRIND_STACK_REGISTER(stack, (char *)stack + STACK_SIZE);
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
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (__builtin_expect(stack_arena == MAP_FAILED, 0))
    {
        perror("mmap stack arena");
        exit(1);
    }

    madvise(stack_arena, (size_t)STACK_CAP * STACK_SIZE, MADV_HUGEPAGE);
    hot_slab = mmap(NULL, (size_t)THREAD_CAP * sizeof(thread_hot_t), PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (__builtin_expect(hot_slab == MAP_FAILED, 0))
    {
        perror("mmap hot slab");
        exit(1);
    }

    cold_slab = mmap(NULL, (size_t)THREAD_CAP * sizeof(thread_cold_t), PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (__builtin_expect(cold_slab == MAP_FAILED, 0))
    {
        perror("mmap cold slab");
        exit(1);
    }

    dedup_mem_init();

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
    dedup_mem_destroy();
    stack_arena = NULL;
    hot_slab = NULL;
    cold_slab = NULL;
}

#define STACK_FREELIST_PTR(base) ((void **)((char *)(base) + STACK_SIZE - sizeof(void *)))

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

    return (char *)stack_arena + ((size_t)stack_arena_next++ * STACK_SIZE);
}

static void stack_release(void *s)
{
    *STACK_FREELIST_PTR(s) = stack_free_head;
    stack_free_head = s;
}

// Free-list intrusive via le champ rsp (inutilisé quand le thread est libéré)
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

static void *exit_rsp;

__attribute__((visibility("hidden"))) void thread_entry(void *(*func)(void *), void *arg)
{
    THREAD_COLD(current)->started = 1;
#ifdef USE_PREEMPTION
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGALRM);
    sigprocmask(SIG_UNBLOCK, &mask, NULL);
#endif
    void *ret = func(arg);
    thread_exit(ret);
    __builtin_unreachable();
}

static int stack_color_counter;

static void *stack_init(void *stack_base, void *(*func)(void *), void *arg)
{
    uintptr_t sp = (uintptr_t)stack_base + STACK_SIZE;

    sp -= ((unsigned)stack_color_counter++ & 0xFF) * 64;

    sp &= ~(uintptr_t)0xF;

    void **s = (void **)sp;

    // Adresse de retour pour context_switch → ret → thread_trampoline
    *(--s) = (void *)thread_trampoline;

    *(--s) = 0;            // rbx
    *(--s) = 0;            // rbp
    *(--s) = (void *)func; // r12 = func
    *(--s) = arg;          // r13 = arg
    *(--s) = 0;            // r14
    *(--s) = 0;            // r15

    VALGRIND_MAKE_MEM_DEFINED(s, 7 * sizeof(void *));

    return s;
}

__attribute__((cold)) static void clean_exit(void)
{
    thread_cold_t *cc = THREAD_COLD(current);
    if (cc->stack_base)
    {
#ifndef NVALGRIND
        VALGRIND_STACK_DEREGISTER(cc->valgrind_stackid);
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
            thread_cold_t *tc = THREAD_COLD(t);
            if (tc->stack_base)
            {
#ifndef NVALGRIND
                VALGRIND_STACK_DEREGISTER(tc->valgrind_stackid);
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
    sched_init();
    last_created = NULL;
    atexit(thread_system_destroy);

    current = thread_alloc();
    if (__builtin_expect(!current, 0))
    {
        perror("thread_alloc main");
        exit(1);
    }
    current->rsp = NULL;
    current->sched_next = current;
    current->sched_prev = current;

    thread_cold_t *cc = THREAD_COLD(current);
    cc->state = READY;
    cc->stack_base = NULL;
    cc->retval = NULL;
    cc->waiting = NULL;
    cc->joining = NULL;
    cc->func = NULL;
    cc->func_arg = NULL;
    cc->inline_jmpbuf = NULL;

    uintptr_t sp = (uintptr_t)(exit_stack + sizeof(exit_stack));
    sp &= ~(uintptr_t)0xF;
    void **s = (void **)sp;
    *(--s) = (void *)clean_exit;
    *(--s) = 0;
    *(--s) = 0;
    *(--s) = 0;
    *(--s) = 0;
    *(--s) = 0;
    *(--s) = 0;
    exit_rsp = s;
    VALGRIND_MAKE_MEM_DEFINED(s, 7 * sizeof(void *));

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

    if (__builtin_expect(dedup_enabled, 1)){
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
    }

    thread_hot_t *t = thread_alloc();
    if (__builtin_expect(!t, 0))
        return -1;

    thread_cold_t *tc = THREAD_COLD(t);
    tc->stack_base = NULL;
    tc->state = READY;
    tc->retval = NULL;
    tc->waiting = NULL;
    tc->joining = NULL;
    tc->func = NULL;
    tc->func_arg = NULL;
    tc->inline_jmpbuf = NULL;
    tc->refcount = 1;
    tc->started = 0;
    t->rsp = NULL;

    flush_last_created();
    last_created = t;
    last_created_func = func;
    last_created_arg = arg;
    *newthread = (thread_t)t;

    preempt_restore(&old); 

    return 0;
}

// thread_yield_asm est implémenté dans context_switch.S

#ifdef USE_PREEMPTION
                                                                                                                                        
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

__attribute__((visibility("default"), hot)) int thread_join(thread_t thread, void **retval)
{
    sigset_t old;
    preempt_block(&old);

    thread_hot_t *t = (thread_hot_t *)thread;
    thread_cold_t *tc = THREAD_COLD(t);

    if (__builtin_expect(tc->state != FINISHED, 1))
    {
        for (thread_hot_t *p = t; p != NULL; p = THREAD_COLD(p)->joining)
        {
            if (p == current)
            {
                preempt_restore(&old);
                return EDEADLK;
            }
        }
        THREAD_COLD(current)->joining = t;

        if (__builtin_expect(last_created == t, 1))
        {
            // Fast path: thread not yet flushed, no stack allocated
            last_created = NULL;

            if (__builtin_expect(t->rsp == NULL, 1))
            {
                thread_hot_t *prev = current;
                current = t;
                sched_replace(prev, t);

                void *(*fn)(void *) = last_created_func;
                void *ar = last_created_arg;

                tc->started = 1;
                inline_executing++;
                jmp_buf jb;
                if (__builtin_expect(_setjmp(jb) == 0, 1))
                {
                    tc->inline_jmpbuf = &jb;
                    tc->retval = fn(ar);
                }
                tc->inline_jmpbuf = NULL;
                tc->state = FINISHED;
                inline_executing--;

                sched_replace(t, prev);
                current = prev;
            }
        }
        else if (__builtin_expect(!tc->started && tc->func != NULL, 0))
        {
            // Generalized inline: thread was flushed but never ran
            flush_last_created();
            sched_remove(t);

            // Release the unused stack
            if (tc->stack_base)
            {
#ifndef NVALGRIND
                VALGRIND_STACK_DEREGISTER(tc->valgrind_stackid);
#endif
                stack_release(tc->stack_base);
                tc->stack_base = NULL;
            }
            t->rsp = NULL;

            thread_hot_t *prev = current;
            current = t;
            sched_replace(prev, t);

            void *(*fn)(void *) = tc->func;
            void *ar = tc->func_arg;

            tc->started = 1;
            inline_executing++;
            jmp_buf jb;
            if (__builtin_expect(_setjmp(jb) == 0, 1))
            {
                tc->inline_jmpbuf = &jb;
                tc->retval = fn(ar);
            }
            tc->inline_jmpbuf = NULL;
            tc->state = FINISHED;
            inline_executing--;

            sched_replace(t, prev);
            current = prev;
        }
        else
        {
            tc->waiting = current;
            flush_last_created();

            if (__builtin_expect(is_sched_empty(), 0)){
                preempt_restore(&old);
                return -1;
            }

            thread_hot_t *next = current->sched_prev;
            sched_remove(current);

            thread_hot_t *prev = current;
            current = next;

            if (__builtin_expect(next->rsp == NULL, 0))
                lazy_stack_alloc(next);

            context_switch(&prev->rsp, next->rsp);
        }
        THREAD_COLD(current)->joining = NULL;
    }

    if (retval)
        *retval = tc->retval;

    // Refcount-guarded cleanup
    tc->refcount--;
    if (__builtin_expect(tc->refcount == 0, 1))
    {
        if (tc->func != NULL)
        {
            if (dedup_enabled)
                dedup_remove(tc->func, tc->func_arg);
            tc->func = NULL;
        }

        if (tc->stack_base)
        {
#ifndef NVALGRIND
            VALGRIND_STACK_DEREGISTER(tc->valgrind_stackid);
#endif
            stack_release(tc->stack_base);
        }

        thread_release(t);
    }
    preempt_restore(&old);

    return 0;
}

__attribute__((visibility("default"), hot)) void thread_exit(void *retval)
{
    sigset_t old;                                                                                                                     
    preempt_block(&old);

    thread_cold_t *cc = THREAD_COLD(current);
    cc->retval = retval;

    if (__builtin_expect(cc->inline_jmpbuf != NULL, 0))
    {
        _longjmp(*cc->inline_jmpbuf, 1);
        __builtin_unreachable();
    }

    cc->state = FINISHED;

    if (__builtin_expect(cc->waiting != NULL, 1))
    {
        thread_hot_t *w = cc->waiting;
        sched_replace(current, w);
        current = w;
        if (__builtin_expect(w->rsp == NULL, 0))
            lazy_stack_alloc(w);
        context_restore(w->rsp);
    }

    if (__builtin_expect(cc->func != NULL, 1))
    {
        if (dedup_enabled)
            dedup_remove(cc->func, cc->func_arg);
        cc->func = NULL;
    }

    flush_last_created();

    if (__builtin_expect(is_sched_empty(), 0))
        context_restore(exit_rsp);

    thread_hot_t *next = current->sched_prev;
    sched_remove(current);
    current = next;

    if (__builtin_expect(next->rsp == NULL, 0))
        lazy_stack_alloc(next);

    context_restore(next->rsp);
}

// Mutex (stubs)

#define MUTEX(m) ((mutex_internal_t *)(m))


__attribute__((visibility("default"))) int thread_mutex_init(thread_mutex_t *mutex)
{
    dedup_enabled = 0;
    mutex_internal_t *m = MUTEX(mutex);
    m->locked = 0;                                                                                                                    
    m->wait_head = NULL;
    m->wait_tail = NULL;                                                                                                              
    return 0;
}


__attribute__((visibility("default"))) int thread_mutex_destroy(thread_mutex_t *mutex)
{
    (void)mutex;
    return 0;
}


__attribute__((visibility("default"), hot)) int thread_mutex_lock(thread_mutex_t *mutex)
{
    sigset_t old;                                                                                                                     
    preempt_block(&old);

    mutex_internal_t *m = MUTEX(mutex);                                                                                               
                                                                                                                                        
    if (__builtin_expect(!m->locked, 1))
    {
        m->locked = 1;
        preempt_restore(&old); // test 72
        return 0;
    }

    if (__builtin_expect(is_sched_empty(), 0)){
        preempt_restore(&old);
        return -1;
    }
                                                                                                                                      
    thread_hot_t *me = current;
    thread_hot_t *next = current->sched_prev;
    sched_remove(current);

    me->sched_next = NULL; 
    if (m->wait_tail == NULL)                                                                                                         
        m->wait_head = me;
    else                                                                                                                              
        m->wait_tail->sched_next = me;
    m->wait_tail = me;                                                                                                                
                                                                                                           
    current = next;
    __builtin_prefetch(next->rsp, 0, 3);
    context_switch(&me->rsp, next->rsp);
    
    preempt_restore(&old);

    return 0;
}


__attribute__((visibility("default"), hot)) int thread_mutex_unlock(thread_mutex_t *mutex)
{
    sigset_t old;                                                                                                                     
    preempt_block(&old);

    mutex_internal_t *m = MUTEX(mutex);                                                                                               
                  
    thread_hot_t *w = m->wait_head;                                                                                                   
    if (w == NULL)
    {                                                                                                                                 
        m->locked = 0;
        preempt_restore(&old);
        return 0;
    }                                                     
    m->wait_head = w->sched_next;
    if (m->wait_head == NULL)                                                                                                         
        m->wait_tail = NULL;
                                                                                                                                      
    sched_enqueue(w);

    preempt_restore(&old);

    return 0;     
}
