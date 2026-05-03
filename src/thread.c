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

static void *stack_arena;
static int stack_arena_next;

/* Hidden (non-static) : thread_yield_asm consomme pending_dead_stack et
 * pousse dans stack_free_head directement, évitant un wrapper C avec
 * call/ret/endbr64 sur le hot path du yield. */
__attribute__((visibility("hidden"))) void *stack_free_head;

/* Stack du dernier thread qui a fait thread_exit, en attente de release.
 * On ne peut pas la stack_release dans thread_exit car le thread courant
 * y exécute encore le code jusqu'au context_restore — la libérer à ce
 * moment permettrait à lazy_stack_alloc(next) de la réutiliser pour next,
 * dont stack_init() écrirait au sommet de cette même pile.
 * Le retour côté yield (post context_switch) la libère réellement. */
__attribute__((visibility("hidden"))) void *pending_dead_stack;

__attribute__((visibility("hidden"))) thread_hot_t  *hot_slab;
__attribute__((visibility("hidden"))) thread_cold_t *cold_slab;
static int thread_slab_next;

static thread_hot_t *thread_free_head;




// La préemption (handler/init locaux ; block/restore dans preempt.h)
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
static void *stack_init(void *stack_base, void *(*func)(void *), void *arg);

__attribute__((noinline, cold, visibility("hidden"))) void flush_last_created_slow(void)
{
    thread_cold_t *lc = THREAD_COLD(last_created);

    // Deferred stack allocation: don't allocate stack here.
    // Stack will be allocated lazily in yield if thread is actually scheduled,
    // or skipped entirely if the thread is inline-joined.
    lc->func = last_created_func;
    lc->func_arg = last_created_arg;
#ifdef FP
    dedup_insert(last_created_func, last_created_arg, last_created);
#endif
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
        /* L'arena de stacks est épuisée. Le fallback "exécuter inline" est
         * cassé (le yield qui nous a appelé va ensuite context_switch dans
         * t->rsp == NULL → crash). Erreur de capacité non récupérable. */
        fprintf(stderr,
                "libthread: stack arena exhausted (max %d threads concurrents)\n",
                STACK_CAP);
        abort();
    }
    tc->stack_base = stack;
    t->rsp = stack_init(stack, tc->func, tc->func_arg);
#ifndef NVALGRIND
    /* Stack utilisable = [stack + GUARD, stack + STACK_SIZE). */
    tc->valgrind_stackid = VALGRIND_STACK_REGISTER(
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
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (__builtin_expect(stack_arena == MAP_FAILED, 0))
    {
        perror("mmap stack arena");
        exit(1);
    }

    PORT_MADV_HUGEPAGE(stack_arena, (size_t)STACK_CAP * STACK_SIZE);
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

/* Pile de sortie pré-armée : context_restore(exit_rsp) déroule jusqu'à
 * clean_exit() quand le dernier thread termine. clean_exit() appelle
 * mem_destroy() qui munmap la stack arena — d'où la nécessité d'une pile
 * indépendante. Sur fallback, la même pile sert de support à un ucontext_t
 * d'exit (taille augmentée pour loger ucontext_t + appels exit handlers). */
#if PORT_FAST_PATH
static char exit_stack[4096] __attribute__((aligned(16)));
#else
static char exit_stack[16 * 1024] __attribute__((aligned(16)));
#endif
static void *exit_rsp;

__attribute__((visibility("hidden"))) void thread_entry(void *(*func)(void *), void *arg)
{
    THREAD_COLD(current)->started = 1;
    preempt_unblock_self();
    void *ret = func(arg);
    thread_exit(ret);
    __builtin_unreachable();
}

static int stack_color_counter;

static void *stack_init(void *stack_base, void *(*func)(void *), void *arg)
{
#if PORT_FAST_PATH
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
#else
    /* Fallback : pas de pose manuelle de registres callee-saved sur la pile.
     * port_make_ctx alloue un ucontext_t juste après la guard page et l'arme
     * pour appeler thread_entry(func, arg). Le `void *` retourné est en fait
     * un `ucontext_t *` opaque. */
    (void)stack_color_counter;
    return port_make_ctx(stack_base, func, arg);
#endif
}

__attribute__((cold, __noreturn__)) static void clean_exit(void)
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
    current->priority = THREAD_PRIORITY_DEFAULT;

    thread_cold_t *cc = THREAD_COLD(current);
    thread_cold_reset(cc);
    cc->started = 1;  /* main tourne déjà */

#if PORT_FAST_PATH
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
#else
    exit_rsp = port_make_simple_ctx(exit_stack, sizeof(exit_stack), clean_exit);
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

    thread_cold_t *tc = THREAD_COLD(t);
    thread_cold_reset(tc);
    t->rsp = NULL;
    t->priority = THREAD_PRIORITY_DEFAULT;

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
    t->priority = priority;                                                                                                           
    return 0;   
}

__attribute__((visibility("default"))) int thread_getpriority(thread_t thread, int *priority)
{
    if (!priority)
        return -1;
    thread_hot_t *t = (thread_hot_t *)thread;
    *priority = t->priority;
    return 0;
}


// thread_yield_asm est implémenté dans context_switch.S
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

    if (best->priority < current->priority)
    {
        preempt_restore(&old);
        return 0;
    }

    thread_hot_t *prev = current;
    current = best;
    if (__builtin_expect(best->rsp == NULL, 0))
        lazy_stack_alloc(best);

    context_switch(&prev->rsp, best->rsp);
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
    /* thread_yield_asm consomme pending_dead_stack lui-même (cf. asm). */
    int r = thread_yield_asm();
    preempt_restore(&old);
    return r;
}

#else

/* Mono sans préemption : alias `thread_yield = thread_yield_asm` posé dans
 * context_switch.S. La consommation de pending_dead_stack est intégrée à
 * l'asm pour éviter le coût call/ret + endbr64 du wrapper C. */

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

    if(a == b) return -1; //deadlock

    while(a->rank < b->rank){
        thread_cold_t *tmp = a;
        a = b;
        b = tmp;
    }

    b->parent = a;
    if(a->rank == b->rank) a->rank++;
    return 0;
}

/* Exécute inline le thread t dans la frame courante, via setjmp/longjmp :
 * - on prend la place de prev=current dans la sched-list (sched_replace)
 * - fn(ar) tourne ; un thread_exit() depuis fn() saute via _longjmp jusqu'au
 *   _setjmp posé ici, ce qui évite de se retrouver coincé dans la stack du
 *   thread inline (pas de pile dédiée allouée).
 * noinline : exigé pour que les locals ne soient pas clobberés par longjmp. */
__attribute__((noinline))
static void inline_run(thread_hot_t *t, void *(*fn)(void *), void *ar)
{
    thread_cold_t *tc = THREAD_COLD(t);
    thread_hot_t *prev = current;
    current = t;
    sched_replace(prev, t);

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

__attribute__((visibility("default"), hot)) int thread_join(thread_t thread, void **retval)
{
    sigset_t old;
    preempt_block(&old);

    thread_hot_t *t = (thread_hot_t *)thread;
    thread_cold_t *tc = THREAD_COLD(t);

    if (__builtin_expect(tc->state != FINISHED, 1))
    {
        if(uf_union(THREAD_COLD(current), tc) == -1) {
            preempt_restore(&old);
            return EDEADLK;
        }

        if (__builtin_expect(last_created == t, 1))
        {
            /* Fast inline-join : t encore en TLS, jamais sched_enqueue, pas
             * de stack allouée. On l'exécute dans notre frame. */
            last_created = NULL;
            if (__builtin_expect(t->rsp == NULL, 1))
                inline_run(t, last_created_func, last_created_arg);
        }
        else if (__builtin_expect(!tc->started && tc->func != NULL, 0))
        {
            /* Generalized inline : t a été flushé (donc dans la sched-list)
             * mais n'a pas encore été élu par le scheduler.
             * Invariant : `tc->func != NULL` n'est posé que par
             * flush_last_created_slow(), qui appelle aussi sched_enqueue(t).
             * Seul le thread lui-même peut se sched_remove (via
             * mutex/sem/sigwait/exit), ce qui requiert started==1. Donc
             * (!tc->started && tc->func != NULL) implique t encore dans
             * la liste circulaire → sched_remove(t) est safe.
             * (Cas user-bug "double-join" exclu, non spécifié par l'API.) */
            flush_last_created();
            sched_remove(t);

            /* La stack a peut-être été pré-allouée par lazy_stack_alloc :
             * on la rend au pool puisqu'on va exécuter inline. */
            if (tc->stack_base)
            {
#ifndef NVALGRIND
                VALGRIND_STACK_DEREGISTER(tc->valgrind_stackid);
#endif
                stack_release(tc->stack_base);
                tc->stack_base = NULL;
            }
            t->rsp = NULL;

            inline_run(t, tc->func, tc->func_arg);
        }
        else
        {
            tc->waiting = current;
            flush_last_created();

            if (__builtin_expect(is_sched_empty(), 0)){
                preempt_restore(&old);
                return -1;
            }
            thread_hot_t *next = sched_pick_next();

            sched_remove(current);

            thread_hot_t *prev = current;
            current = next;

            if (__builtin_expect(next->rsp == NULL, 0))
                lazy_stack_alloc(next);

            context_switch(&prev->rsp, next->rsp);
        }
    }

    if (retval)
        *retval = tc->retval;

    // Refcount-guarded cleanup
    tc->refcount--;
    if (__builtin_expect(tc->refcount == 0, 1))
    {
        if (tc->func != NULL)
        {
#ifdef FP
            dedup_remove(tc->func, tc->func_arg);
#endif
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
#ifdef FP
        dedup_remove(cc->func, cc->func_arg);
#endif
        cc->func = NULL;
    }

    flush_last_created();

    if (__builtin_expect(is_sched_empty(), 0))
        context_restore(exit_rsp);

    thread_hot_t *next = sched_pick_next();

    sched_remove(current);

    /* Différer la release de notre stack : on l'utilise encore jusqu'au
     * context_restore. La précédente pending_dead_stack (s'il y en a une,
     * cas de deux thread_exit consécutifs sans yield au main) est libérée
     * dès maintenant — son thread est mort, lazy_stack_alloc(next) peut la
     * réutiliser sans risque. */
    if (pending_dead_stack)
        stack_release(pending_dead_stack);
    pending_dead_stack = cc->stack_base;
    cc->stack_base = NULL;

    current = next;

    if (__builtin_expect(next->rsp == NULL, 0))
        lazy_stack_alloc(next);

    context_restore(next->rsp);
}

/* Mutex, sémaphores, signaux thread : implémentations dans thread_sync.c
 * (partagé avec la variante MN). */

/* Wrapper hidden vers le flush_last_created statique inline ci-dessus.
 * Permet à thread_sync.c (compilé séparément) d'invoquer le flush sans
 * dupliquer la logique. */
__attribute__((visibility("hidden"))) void flush_last_created_pub(void)
{
    flush_last_created();
}
