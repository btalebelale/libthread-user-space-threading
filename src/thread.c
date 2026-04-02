#pragma GCC optimize("Ofast,unroll-loops")

#include "thread.h"
#include "scheduler.h"

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/mman.h>
#include <valgrind/valgrind.h>

// 64 KiB par stack thread (demand-paged : seules les pages touchées consomment de la RAM)
#define STACK_SIZE  (64 * 1024)
// Capacité max de l'arène (threads simultanés). 16384 * 64K = 1GB d'espace virtuel.
// Demand-paging : seules les pages touchées consomment de la RAM physique.
#define ARENA_CAP   16384

// ===========================================================================
// Sous-système mémoire : arène de stacks + slabs hot/cold
// Deux slabs parallèles indexés de la même façon :
//   hot_slab[i]  = données accédées à chaque yield (rsp, state)
//   cold_slab[i] = données accédées rarement (retval, stack_base, waiting, ...)
// Macro THREAD_COLD(hot_ptr) : O(1) pointer arithmetic pour passer de hot à cold.
// ===========================================================================

// Arène de stacks : région mmap'd contiguë découpée en blocs de STACK_SIZE
static void *stack_arena;
static int   stack_arena_next;
// Free-list intrusive : le premier mot de chaque stack libéré pointe au suivant
static void *stack_free_head;

// Slabs parallèles hot/cold
static thread_hot_t  *hot_slab;
static thread_cold_t *cold_slab;
static int            thread_slab_next;
// Free-list intrusive : on réutilise le champ 'rsp' du hot struct libéré
static thread_hot_t  *thread_free_head;

// Accès cold depuis un pointeur hot : O(1) via index dans le slab
#define THREAD_COLD(hot) (&cold_slab[(hot) - hot_slab])

__attribute__((cold))
static void mem_init(void)
{
    stack_arena = mmap(NULL, (size_t)ARENA_CAP * STACK_SIZE,
                       PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (__builtin_expect(stack_arena == MAP_FAILED, 0))
    {
        perror("mmap stack arena");
        exit(1);
    }

    hot_slab = mmap(NULL, (size_t)ARENA_CAP * sizeof(thread_hot_t),
                    PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (__builtin_expect(hot_slab == MAP_FAILED, 0))
    {
        perror("mmap hot slab");
        exit(1);
    }

    cold_slab = mmap(NULL, (size_t)ARENA_CAP * sizeof(thread_cold_t),
                     PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (__builtin_expect(cold_slab == MAP_FAILED, 0))
    {
        perror("mmap cold slab");
        exit(1);
    }

    stack_arena_next = 0;
    stack_free_head = NULL;
    thread_slab_next = 0;
    thread_free_head = NULL;
}

__attribute__((cold))
static void mem_destroy(void)
{
    if (stack_arena && stack_arena != MAP_FAILED)
        munmap(stack_arena, (size_t)ARENA_CAP * STACK_SIZE);
    if (hot_slab && hot_slab != MAP_FAILED)
        munmap(hot_slab, (size_t)ARENA_CAP * sizeof(thread_hot_t));
    if (cold_slab && cold_slab != MAP_FAILED)
        munmap(cold_slab, (size_t)ARENA_CAP * sizeof(thread_cold_t));
    stack_arena = NULL;
    hot_slab = NULL;
    cold_slab = NULL;
}

static void *stack_alloc(void)
{
    if (__builtin_expect(stack_free_head != NULL, 1))
    {
        void *s = stack_free_head;
        stack_free_head = *(void **)s;
        return s;
    }
    if (__builtin_expect(stack_arena_next >= ARENA_CAP, 0))
        return NULL;

    return (char *)stack_arena + ((size_t)stack_arena_next++ * STACK_SIZE);
}

static void stack_release(void *s)
{
    *(void **)s = stack_free_head;
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
    if (__builtin_expect(thread_slab_next >= ARENA_CAP, 0))
        return NULL;
    return &hot_slab[thread_slab_next++];
}

static void thread_release(thread_hot_t *t)
{
    t->rsp = (void *)thread_free_head;
    thread_free_head = t;
}

// ===========================================================================
// Stack dédié pour clean_exit
// ===========================================================================
static char exit_stack[4096] __attribute__((aligned(16)));
// Frame pré-calculé pour clean_exit (évite de recalculer à chaque thread_exit)
static void *exit_rsp;

// Point d'entrée C pour les nouveaux threads, appelé depuis thread_trampoline (asm)
__attribute__((visibility("hidden")))
void thread_entry(void *(*func)(void *), void *arg)
{
    void *ret = func(arg);
    thread_exit(ret);
    __builtin_unreachable();
}

// Initialise le stack d'un nouveau thread pour context_switch.
static void *stack_init(void *stack_base, void *(*func)(void *), void *arg)
{
    uintptr_t sp = (uintptr_t)stack_base + STACK_SIZE;
    sp &= ~(uintptr_t)0xF; // alignement 16 bytes

    void **s = (void **)sp;

    // Adresse de retour pour context_switch → ret → thread_trampoline
    *(--s) = (void *)thread_trampoline;

    // Registres callee-saved (pop order: r15, r14, r13, r12, rbp, rbx)
    *(--s) = 0;             // rbx
    *(--s) = 0;             // rbp
    *(--s) = (void *)func;  // r12 = func
    *(--s) = arg;           // r13 = arg
    *(--s) = 0;             // r14
    *(--s) = 0;             // r15

    return s;
}

// ===========================================================================
// Cycle de vie du système de threads
// ===========================================================================

__attribute__((cold))
static void clean_exit(void)
{
    thread_cold_t *cc = THREAD_COLD(current);
    if (cc->stack_base)
        VALGRIND_STACK_DEREGISTER(cc->valgrind_stackid);
    current = NULL;
    sched_cleanup();
    mem_destroy();
    exit(0);
}

__attribute__((cold))
static void thread_system_destroy(void)
{
    while (!is_sched_empty())
    {
        thread_hot_t *t = sched_dequeue();
        thread_cold_t *tc = THREAD_COLD(t);
        if (tc->stack_base)
            VALGRIND_STACK_DEREGISTER(tc->valgrind_stackid);
    }
    current = NULL;
    sched_cleanup();
    mem_destroy();
}

__attribute__((cold))
static void init_system(void)
{
    mem_init();
    sched_init();
    atexit(thread_system_destroy);

    current = thread_alloc();
    if (__builtin_expect(!current, 0))
    {
        perror("thread_alloc main");
        exit(1);
    }
    current->state = RUNNING;
    current->rsp = NULL;

    thread_cold_t *cc = THREAD_COLD(current);
    cc->stack_base = NULL;
    cc->retval = NULL;
    cc->waiting = NULL;

    // Pré-calculer le frame exit_stack une seule fois
    uintptr_t sp = (uintptr_t)(exit_stack + sizeof(exit_stack));
    sp &= ~(uintptr_t)0xF;
    void **s = (void **)sp;
    *(--s) = (void *)clean_exit;
    *(--s) = 0; // rbx
    *(--s) = 0; // rbp
    *(--s) = 0; // r12
    *(--s) = 0; // r13
    *(--s) = 0; // r14
    *(--s) = 0; // r15
    exit_rsp = s;
}

// ===========================================================================
// API publique
// ===========================================================================

__attribute__((visibility("default")))
thread_t thread_self(void)
{
    if (__builtin_expect(current == NULL, 0))
        init_system();
    return (thread_t)current;
}

__attribute__((visibility("default")))
int thread_create(thread_t *newthread, void *(*func)(void *), void *arg)
{
    if (__builtin_expect(current == NULL, 0))
        init_system();

    thread_hot_t *t = thread_alloc();
    if (__builtin_expect(!t, 0))
        return -1;

    void *stack = stack_alloc();
    if (__builtin_expect(!stack, 0))
    {
        thread_release(t);
        return -1;
    }

    thread_cold_t *tc = THREAD_COLD(t);
    tc->stack_base = stack;
    t->state = READY;
    tc->retval = NULL;
    tc->waiting = NULL;
    t->rsp = stack_init(stack, func, arg);

    tc->valgrind_stackid = VALGRIND_STACK_REGISTER(
        stack, (char *)stack + STACK_SIZE);

    sched_enqueue(t);
    *newthread = (thread_t)t;
    return 0;
}

// Hot path : aucun accès cold
__attribute__((visibility("default"), hot))
int thread_yield(void)
{
    if (__builtin_expect(current == NULL, 0))
        init_system();

    if (__builtin_expect(is_sched_empty(), 0))
        return 0;

    thread_hot_t *prev = current;
    prev->state = READY;
    sched_enqueue(prev);

    thread_hot_t *next = sched_dequeue();
    next->state = RUNNING;
    current = next;

    __builtin_prefetch(next->rsp, 0, 3);
    context_switch(&prev->rsp, next->rsp);
    return 0;
}

__attribute__((visibility("default"), hot))
int thread_join(thread_t thread, void **retval)
{
    thread_hot_t *t = (thread_hot_t *)thread;
    thread_cold_t *tc = THREAD_COLD(t);

    if (__builtin_expect(t->state != FINISHED, 1))
    {
        tc->waiting = current;
        current->state = BLOCKED;

        thread_hot_t *next = sched_dequeue();
        if (__builtin_expect(!next, 0))
            return -1;

        next->state = RUNNING;
        thread_hot_t *prev = current;
        current = next;

        __builtin_prefetch(next->rsp, 0, 3);
        context_switch(&prev->rsp, next->rsp);
    }

    if (retval)
        *retval = tc->retval;

    if (tc->stack_base)
    {
        VALGRIND_STACK_DEREGISTER(tc->valgrind_stackid);
        stack_release(tc->stack_base);
    }

    thread_release(t);
    return 0;
}

__attribute__((visibility("default"), hot))
void thread_exit(void *retval)
{
    thread_cold_t *cc = THREAD_COLD(current);
    cc->retval = retval;
    current->state = FINISHED;

    if (__builtin_expect(cc->waiting != NULL, 0))
    {
        cc->waiting->state = READY;
        sched_enqueue(cc->waiting);
    }

    if (__builtin_expect(is_sched_empty(), 0))
        context_restore(exit_rsp);

    thread_hot_t *next = sched_dequeue();
    next->state = RUNNING;
    current = next;

    context_restore(next->rsp);
}

// ===========================================================================
// Mutex (stubs)
// ===========================================================================

__attribute__((visibility("default")))
int thread_mutex_init(thread_mutex_t *mutex)    { (void)mutex; return 0; }
__attribute__((visibility("default")))
int thread_mutex_destroy(thread_mutex_t *mutex)  { (void)mutex; return 0; }
__attribute__((visibility("default")))
int thread_mutex_lock(thread_mutex_t *mutex)     { (void)mutex; return 0; }
__attribute__((visibility("default")))
int thread_mutex_unlock(thread_mutex_t *mutex)   { (void)mutex; return 0; }
