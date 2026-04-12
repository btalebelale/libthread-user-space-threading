#pragma GCC optimize("Ofast,unroll-loops")

#include "thread.h"
#include "scheduler.h"

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/mman.h>
#ifndef NVALGRIND
#include <valgrind/valgrind.h>
#include <valgrind/memcheck.h>
#else
#define VALGRIND_STACK_REGISTER(start, end) (0)
#define VALGRIND_STACK_DEREGISTER(id) ((void)(id))
#define VALGRIND_MAKE_MEM_DEFINED(addr, len) ((void)0)
#endif

#ifndef STACK_SIZE
#define STACK_SIZE  (64 * 1024)
#endif

// Nombre max de stacks dans l'arena
// Ajuste automatiquement pour occuper ~1GB max d'espace virtuel
#define STACK_CAP   (1073741824 / STACK_SIZE)

#define THREAD_CAP  262144


static void *stack_arena;
static int   stack_arena_next;

static void *stack_free_head;


static thread_hot_t  *hot_slab;
static thread_cold_t *cold_slab;
static int            thread_slab_next;

static thread_hot_t  *thread_free_head;

// Lazy enqueue : dernier thread cree mais pas encore enqueue
// Flush dans yield/exit/create pour garantir la coherence
static thread_hot_t  *last_created;

#define THREAD_COLD(hot) (&cold_slab[(hot) - hot_slab])

static inline __attribute__((always_inline))
void flush_last_created(void)
{
    if (__builtin_expect(last_created != NULL, 0))
    {
        sched_enqueue(last_created);
        last_created = NULL;
    }
}

__attribute__((cold))
static void mem_init(void)
{
    stack_arena = mmap(NULL, (size_t)STACK_CAP * STACK_SIZE,
                       PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (__builtin_expect(stack_arena == MAP_FAILED, 0))
    {
        perror("mmap stack arena");
        exit(1);
    }
    hot_slab = mmap(NULL, (size_t)THREAD_CAP * sizeof(thread_hot_t),
                    PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (__builtin_expect(hot_slab == MAP_FAILED, 0))
    {
        perror("mmap hot slab");
        exit(1);
    }

    cold_slab = mmap(NULL, (size_t)THREAD_CAP * sizeof(thread_cold_t),
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
        munmap(stack_arena, (size_t)STACK_CAP * STACK_SIZE);
    if (hot_slab && hot_slab != MAP_FAILED)
        munmap(hot_slab, (size_t)THREAD_CAP * sizeof(thread_hot_t));
    if (cold_slab && cold_slab != MAP_FAILED)
        munmap(cold_slab, (size_t)THREAD_CAP * sizeof(thread_cold_t));
    stack_arena = NULL;
    hot_slab = NULL;
    cold_slab = NULL;
}

// Free-list intrusive via le TOP du stack (page deja faultée par l'execution)
// Evite un page fault sur la page base (jamais touchee avec 64KB stacks)
#define STACK_FREELIST_PTR(base) \
    ((void **)((char *)(base) + STACK_SIZE - sizeof(void *)))

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

// Stack dédié pour clean_exit
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

// Compteur pour le stack coloring — distribue les stack tops
// sur differents sets cache L1/L2 pour eviter l'aliasing catastrophique
// (tous les stacks sont STACK_SIZE-alignes = meme set sans coloring)
static int stack_color_counter;

// Initialise le stack d'un nouveau thread pour context_switch.
static void *stack_init(void *stack_base, void *(*func)(void *), void *arg)
{
    uintptr_t sp = (uintptr_t)stack_base + STACK_SIZE;

    // Stack coloring : offset de (index % 256) * 64 bytes
    // Avec STACK_SIZE = 64KB (puissance de 2), tous les stack tops
    // mappent sur le meme set L1/L2. Cet offset les distribue sur
    // 256 sets differents, couvrant L1 (64 sets) et L2 (1024 sets).
    // Pour 1000 threads : ~4 par set L2 (4-way) → fits.
    // Cout max : 255*64 = 16320 bytes (~25% de 64KB)
    sp -= ((unsigned)stack_color_counter++ & 0xFF) * 64;

    sp &= ~(uintptr_t)0xF; // alignement 16 bytes

    void **s = (void **)sp;

    // Adresse de retour pour context_switch → ret → thread_trampoline
    *(--s) = (void *)thread_trampoline;

    *(--s) = 0;             // rbx
    *(--s) = 0;             // rbp
    *(--s) = (void *)func;  // r12 = func
    *(--s) = arg;           // r13 = arg
    *(--s) = 0;             // r14
    *(--s) = 0;             // r15

    // Indique a valgrind que le frame initial est defini
    // (sinon les pop dans context_switch reportent des "uninitialised value")
    VALGRIND_MAKE_MEM_DEFINED(s, 7 * sizeof(void *));

    return s;
}

// Cycle de vie du système de threads

__attribute__((cold))
static void clean_exit(void)
{
    thread_cold_t *cc = THREAD_COLD(current);
    if (cc->stack_base) {
#ifndef NVALGRIND
        VALGRIND_STACK_DEREGISTER(cc->valgrind_stackid);
#endif
    }
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
        thread_hot_t *t = sched_dequeue_fifo();
        thread_cold_t *tc = THREAD_COLD(t);
        if (tc->stack_base) {
#ifndef NVALGRIND
            VALGRIND_STACK_DEREGISTER(tc->valgrind_stackid);
#endif
        }
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
    last_created = NULL;
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
    VALGRIND_MAKE_MEM_DEFINED(s, 7 * sizeof(void *));
}

// API publique


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
        thread_cold_t *tc = THREAD_COLD(t);
        tc->stack_base = NULL;
        tc->waiting = NULL;
        tc->retval = func(arg);
        t->state = FINISHED;
        *newthread = (thread_t)t;
        return 0;
    }

    thread_cold_t *tc = THREAD_COLD(t);
    tc->stack_base = stack;
    t->state = READY;
    tc->retval = NULL;
    tc->waiting = NULL;
    t->rsp = stack_init(stack, func, arg);

#ifndef NVALGRIND
    tc->valgrind_stackid = VALGRIND_STACK_REGISTER(
        stack, (char *)stack + STACK_SIZE);
#endif

    // Lazy enqueue : flush le precedent, garder le nouveau en attente
    flush_last_created();
    last_created = t;
    *newthread = (thread_t)t;
    return 0;
}

// Hot path : aucun accès cold
__attribute__((visibility("default"), hot))
int thread_yield(void)
{
    if (__builtin_expect(current == NULL, 0))
        init_system();

    flush_last_created();

    if (__builtin_expect(is_sched_empty(), 0))
        return 0;

    thread_hot_t *prev = current;
    // Pas de prev->state = READY ni next->state = RUNNING ici :
    // le seul lecteur de state est thread_join (test != FINISHED)
    // et READY != FINISHED, donc l'invariant est respecte.
    // Economise 2 stores par yield + evite de dirty la cache line de prev.
    sched_enqueue(prev);

    thread_hot_t *next = sched_dequeue_fifo();
    current = next;

    VALGRIND_MAKE_MEM_DEFINED(next->rsp, 7 * sizeof(void *));
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

        thread_hot_t *next;
        if (__builtin_expect(last_created == t, 1))
        {
            // Fast path : switch direct vers le thread qu'on vient de creer
            next = t;
            last_created = NULL;
        }
        else
        {
            // Slow path : flush et passer par le scheduler
            flush_last_created();
            next = sched_dequeue_lifo();
            if (__builtin_expect(!next, 0))
                return -1;
        }

        next->state = RUNNING;
        thread_hot_t *prev = current;
        current = next;

        __builtin_prefetch(next->rsp, 0, 3);
        VALGRIND_MAKE_MEM_DEFINED(next->rsp, 7 * sizeof(void *));
        context_switch(&prev->rsp, next->rsp);
    }

    if (retval)
        *retval = tc->retval;

    if (tc->stack_base)
    {
#ifndef NVALGRIND
        VALGRIND_STACK_DEREGISTER(tc->valgrind_stackid);
#endif
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

    if (__builtin_expect(cc->waiting != NULL, 1))
    {
        // Direct handoff : skip le scheduler, restore direct au waiter
        cc->waiting->state = RUNNING;
        current = cc->waiting;
        VALGRIND_MAKE_MEM_DEFINED(cc->waiting->rsp, 7 * sizeof(void *));
        context_restore(cc->waiting->rsp);
    }

    flush_last_created();

    if (__builtin_expect(is_sched_empty(), 0)) {
        VALGRIND_MAKE_MEM_DEFINED(exit_rsp, 7 * sizeof(void *));
        context_restore(exit_rsp);
    }

    thread_hot_t *next = sched_dequeue_lifo();
    next->state = RUNNING;
    current = next;

    VALGRIND_MAKE_MEM_DEFINED(next->rsp, 7 * sizeof(void *));
    context_restore(next->rsp);
}


// Mutex (stubs)


__attribute__((visibility("default")))
int thread_mutex_init(thread_mutex_t *mutex)    { (void)mutex; return 0; }
__attribute__((visibility("default")))
int thread_mutex_destroy(thread_mutex_t *mutex)  { (void)mutex; return 0; }
__attribute__((visibility("default")))
int thread_mutex_lock(thread_mutex_t *mutex)     { (void)mutex; return 0; }
__attribute__((visibility("default")))
int thread_mutex_unlock(thread_mutex_t *mutex)   { (void)mutex; return 0; }
