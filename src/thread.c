#pragma GCC optimize("Ofast,unroll-loops")

#include "thread.h"
#include "scheduler.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#ifdef DEDUP_SWISS
#include <string.h>
#ifdef __SSE2__
#include <emmintrin.h>
#endif
#endif
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

// Nombre max de stacks dans l'arena
// Ajuste automatiquement pour occuper ~1GB max d'espace virtuel
#define STACK_CAP (1073741824 / STACK_SIZE)

#define THREAD_CAP 262144

static void *stack_arena;
static int stack_arena_next;

static void *stack_free_head;

static thread_hot_t *hot_slab;
static thread_cold_t *cold_slab;
static int thread_slab_next;

static thread_hot_t *thread_free_head;

static thread_hot_t *last_created;

#define DEDUP_BITS 18
#define DEDUP_CAP (1 << DEDUP_BITS)
#define DEDUP_MAGIC 11400714819323198485ULL

typedef void *(*func_ptr_t)(void *);

#ifdef DEDUP_SWISS

/* ── Swiss Table ────────────────────────────────────────────────────── */

#define SWISS_GROUP_SIZE 16
#define SWISS_GROUPS (DEDUP_CAP / SWISS_GROUP_SIZE)
#define SWISS_GROUP_MASK (SWISS_GROUPS - 1)

#define CTRL_EMPTY ((int8_t)0x80)
#define CTRL_DELETED ((int8_t)0xFE)

static int8_t *swiss_ctrl;
static func_ptr_t *swiss_key_func;
static void **swiss_key_arg;
static thread_hot_t **swiss_val;

/* Hash : 64 bits complets */
static inline __attribute__((always_inline)) uint64_t dedup_hash_full(func_ptr_t func, void *arg)
{
    uint64_t h = (uint64_t)(uintptr_t)func * DEDUP_MAGIC;
    h ^= (uint64_t)(uintptr_t)arg * 7046029254386353131ULL;
    return h;
}

/* Bits 63..50 → group index (14 bits) */
static inline __attribute__((always_inline)) uint32_t swiss_group_index(uint64_t h)
{
    return (uint32_t)(h >> 50) & SWISS_GROUP_MASK;
}

/* Bits 49..43 → 7-bit tag (MSB always 0 → occupied) */
static inline __attribute__((always_inline)) int8_t swiss_tag(uint64_t h)
{
    return (int8_t)((h >> 43) & 0x7F);
}

/* ── SSE2 group primitives (scalar fallback) ────────────────────────── */

#ifdef __SSE2__

static inline __attribute__((always_inline)) uint32_t swiss_match(const int8_t *ctrl, int8_t tag)
{
    __m128i group = _mm_load_si128((const __m128i *)ctrl);
    __m128i cmp = _mm_cmpeq_epi8(group, _mm_set1_epi8(tag));
    return (uint32_t)_mm_movemask_epi8(cmp);
}

static inline __attribute__((always_inline)) uint32_t swiss_match_empty(const int8_t *ctrl)
{
    __m128i group = _mm_load_si128((const __m128i *)ctrl);
    __m128i cmp = _mm_cmpeq_epi8(group, _mm_set1_epi8(CTRL_EMPTY));
    return (uint32_t)_mm_movemask_epi8(cmp);
}

static inline __attribute__((always_inline)) uint32_t
swiss_match_empty_or_deleted(const int8_t *ctrl)
{
    __m128i group = _mm_load_si128((const __m128i *)ctrl);
    return (uint32_t)_mm_movemask_epi8(group);
}

#else /* scalar fallback */

static inline __attribute__((always_inline)) uint32_t swiss_match(const int8_t *ctrl, int8_t tag)
{
    uint32_t mask = 0;
    for (int i = 0; i < 16; i++)
        if (ctrl[i] == tag)
            mask |= (1u << i);
    return mask;
}

static inline __attribute__((always_inline)) uint32_t swiss_match_empty(const int8_t *ctrl)
{
    uint32_t mask = 0;
    for (int i = 0; i < 16; i++)
        if (ctrl[i] == CTRL_EMPTY)
            mask |= (1u << i);
    return mask;
}

static inline __attribute__((always_inline)) uint32_t
swiss_match_empty_or_deleted(const int8_t *ctrl)
{
    uint32_t mask = 0;
    for (int i = 0; i < 16; i++)
        if (ctrl[i] < 0)
            mask |= (1u << i);
    return mask;
}

#endif /* __SSE2__ */

/* ── Probe helpers ──────────────────────────────────────────────────── */

typedef struct
{
    uint32_t group;
    uint32_t stride;
} swiss_probe_t;

static inline __attribute__((always_inline)) swiss_probe_t swiss_probe_start(uint64_t h)
{
    return (swiss_probe_t){swiss_group_index(h), 0};
}

static inline __attribute__((always_inline)) void swiss_probe_next(swiss_probe_t *p)
{
    p->stride++;
    p->group = (p->group + p->stride) & SWISS_GROUP_MASK;
}

/* ── dedup operations ───────────────────────────────────────────────── */

static inline __attribute__((always_inline)) thread_hot_t *dedup_lookup(func_ptr_t func, void *arg)
{
    uint64_t h = dedup_hash_full(func, arg);
    int8_t tag = swiss_tag(h);
    swiss_probe_t probe = swiss_probe_start(h);

    for (;;)
    {
        int8_t *ctrl = swiss_ctrl + (probe.group * SWISS_GROUP_SIZE);
        uint32_t base = probe.group * SWISS_GROUP_SIZE;

        uint32_t match = swiss_match(ctrl, tag);
        while (match)
        {
            uint32_t i = (uint32_t)__builtin_ctz(match);
            uint32_t slot = base + i;
            if (swiss_key_func[slot] == func && swiss_key_arg[slot] == arg)
                return swiss_val[slot];
            match &= match - 1;
        }

        if (__builtin_expect(swiss_match_empty(ctrl) != 0, 1))
            return NULL;

        swiss_probe_next(&probe);
    }
}

static inline __attribute__((always_inline)) void dedup_insert(func_ptr_t func, void *arg,
                                                               thread_hot_t *t)
{
    uint64_t h = dedup_hash_full(func, arg);
    int8_t tag = swiss_tag(h);
    swiss_probe_t probe = swiss_probe_start(h);

    for (;;)
    {
        int8_t *ctrl = swiss_ctrl + (probe.group * SWISS_GROUP_SIZE);
        uint32_t base = probe.group * SWISS_GROUP_SIZE;

        uint32_t avail = swiss_match_empty_or_deleted(ctrl);
        if (avail)
        {
            uint32_t i = (uint32_t)__builtin_ctz(avail);
            uint32_t slot = base + i;
            ctrl[i] = tag;
            swiss_key_func[slot] = func;
            swiss_key_arg[slot] = arg;
            swiss_val[slot] = t;
            return;
        }

        swiss_probe_next(&probe);
    }
}

static inline void dedup_remove(func_ptr_t func, void *arg)
{
    uint64_t h = dedup_hash_full(func, arg);
    int8_t tag = swiss_tag(h);
    swiss_probe_t probe = swiss_probe_start(h);

    for (;;)
    {
        int8_t *ctrl = swiss_ctrl + (probe.group * SWISS_GROUP_SIZE);
        uint32_t base = probe.group * SWISS_GROUP_SIZE;

        uint32_t match = swiss_match(ctrl, tag);
        while (match)
        {
            uint32_t i = (uint32_t)__builtin_ctz(match);
            uint32_t slot = base + i;
            if (swiss_key_func[slot] == func && swiss_key_arg[slot] == arg)
            {
                uint32_t empty = swiss_match_empty(ctrl);
                ctrl[i] = empty ? CTRL_EMPTY : CTRL_DELETED;
                swiss_val[slot] = NULL;
                return;
            }
            match &= match - 1;
        }

        if (__builtin_expect(swiss_match_empty(ctrl) != 0, 1))
            return;

        swiss_probe_next(&probe);
    }
}

#else /* !DEDUP_SWISS — linear probing SoA (original) */

#define DEDUP_MASK (DEDUP_CAP - 1)
#define DEDUP_SHIFT (64 - DEDUP_BITS)

static func_ptr_t *dedup_key_func;
static void **dedup_key_arg;
static thread_hot_t **dedup_val;

static inline __attribute__((always_inline)) uint32_t dedup_hash(func_ptr_t func, void *arg)
{
    uint64_t h = (uint64_t)(uintptr_t)func * DEDUP_MAGIC;
    h ^= (uint64_t)(uintptr_t)arg * 7046029254386353131ULL;
    return (uint32_t)(h >> DEDUP_SHIFT);
}

static inline __attribute__((always_inline)) thread_hot_t *dedup_lookup(func_ptr_t func, void *arg)
{
    uint32_t h = dedup_hash(func, arg);
    for (;;)
    {
        if (__builtin_expect(dedup_val[h] == NULL, 0))
            return NULL;
        if (dedup_key_func[h] == func && dedup_key_arg[h] == arg)
            return dedup_val[h];
        h = (h + 1) & DEDUP_MASK;
    }
}

static inline __attribute__((always_inline)) void dedup_insert(func_ptr_t func, void *arg,
                                                               thread_hot_t *t)
{
    uint32_t h = dedup_hash(func, arg);
    while (dedup_val[h] != NULL)
        h = (h + 1) & DEDUP_MASK;
    dedup_key_func[h] = func;
    dedup_key_arg[h] = arg;
    dedup_val[h] = t;
}

static inline void dedup_remove(func_ptr_t func, void *arg)
{
    uint32_t h = dedup_hash(func, arg);
    while (!(dedup_key_func[h] == func && dedup_key_arg[h] == arg))
    {
        if (__builtin_expect(dedup_val[h] == NULL, 0))
            return;
        h = (h + 1) & DEDUP_MASK;
    }

    for (;;)
    {
        uint32_t next = (h + 1) & DEDUP_MASK;
        if (dedup_val[next] == NULL)
            break;
        uint32_t ideal = dedup_hash(dedup_key_func[next], dedup_key_arg[next]);
        if (((h - ideal) & DEDUP_MASK) >= ((next - ideal) & DEDUP_MASK))
            break;
        dedup_key_func[h] = dedup_key_func[next];
        dedup_key_arg[h] = dedup_key_arg[next];
        dedup_val[h] = dedup_val[next];
        h = next;
    }
    dedup_val[h] = NULL;
}

#endif /* DEDUP_SWISS */

#define THREAD_COLD(hot) (&cold_slab[(hot) - hot_slab])

static inline __attribute__((always_inline)) void flush_last_created(void)
{
    if (__builtin_expect(last_created != NULL, 0))
    {
        sched_enqueue(last_created);
        last_created = NULL;
    }
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

#ifdef DEDUP_SWISS
    swiss_ctrl =
        mmap(NULL, (size_t)DEDUP_CAP, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    swiss_key_func = mmap(NULL, (size_t)DEDUP_CAP * sizeof(func_ptr_t), PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    swiss_key_arg = mmap(NULL, (size_t)DEDUP_CAP * sizeof(void *), PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    swiss_val = mmap(NULL, (size_t)DEDUP_CAP * sizeof(thread_hot_t *), PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (__builtin_expect(swiss_ctrl == MAP_FAILED || swiss_key_func == MAP_FAILED ||
                             swiss_key_arg == MAP_FAILED || swiss_val == MAP_FAILED,
                         0))
    {
        perror("mmap dedup swiss");
        exit(1);
    }
    memset(swiss_ctrl, CTRL_EMPTY, DEDUP_CAP);
#else
    dedup_key_func = mmap(NULL, (size_t)DEDUP_CAP * sizeof(func_ptr_t), PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    dedup_key_arg = mmap(NULL, (size_t)DEDUP_CAP * sizeof(void *), PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    dedup_val = mmap(NULL, (size_t)DEDUP_CAP * sizeof(thread_hot_t *), PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (__builtin_expect(dedup_key_func == MAP_FAILED || dedup_key_arg == MAP_FAILED ||
                             dedup_val == MAP_FAILED,
                         0))
    {
        perror("mmap dedup SoA");
        exit(1);
    }
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
#ifdef DEDUP_SWISS
    if (swiss_ctrl && swiss_ctrl != MAP_FAILED)
        munmap(swiss_ctrl, (size_t)DEDUP_CAP);
    if (swiss_key_func && swiss_key_func != MAP_FAILED)
        munmap(swiss_key_func, (size_t)DEDUP_CAP * sizeof(func_ptr_t));
    if (swiss_key_arg && swiss_key_arg != MAP_FAILED)
        munmap(swiss_key_arg, (size_t)DEDUP_CAP * sizeof(void *));
    if (swiss_val && swiss_val != MAP_FAILED)
        munmap(swiss_val, (size_t)DEDUP_CAP * sizeof(thread_hot_t *));
    swiss_ctrl = NULL;
    swiss_key_func = NULL;
    swiss_key_arg = NULL;
    swiss_val = NULL;
#else
    if (dedup_key_func && dedup_key_func != MAP_FAILED)
        munmap(dedup_key_func, (size_t)DEDUP_CAP * sizeof(func_ptr_t));
    if (dedup_key_arg && dedup_key_arg != MAP_FAILED)
        munmap(dedup_key_arg, (size_t)DEDUP_CAP * sizeof(void *));
    if (dedup_val && dedup_val != MAP_FAILED)
        munmap(dedup_val, (size_t)DEDUP_CAP * sizeof(thread_hot_t *));
    dedup_key_func = NULL;
    dedup_key_arg = NULL;
    dedup_val = NULL;
#endif
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

    thread_cold_t *cc = THREAD_COLD(current);
    cc->state = RUNNING;
    cc->stack_base = NULL;
    cc->retval = NULL;
    cc->waiting = NULL;
    cc->func = NULL;
    cc->func_arg = NULL;

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
}

__attribute__((visibility("default"))) thread_t thread_self(void)
{
    return (thread_t)current;
}

__attribute__((visibility("default"))) int thread_create(thread_t *newthread, void *(*func)(void *),
                                                         void *arg)
{

    if (__builtin_expect(last_created != NULL, 0))
    {
        thread_cold_t *lc = THREAD_COLD(last_created);
        if (lc->func == func && lc->func_arg == arg)
        {
            *newthread = (thread_t)last_created;
            return 0;
        }
    }
    {
        thread_hot_t *existing = dedup_lookup(func, arg);
        if (__builtin_expect(existing != NULL, 0))
        {
            *newthread = (thread_t)existing;
            return 0;
        }
    }

    thread_hot_t *t = thread_alloc();
    if (__builtin_expect(!t, 0))
        return -1;

    void *stack = stack_alloc();
    if (__builtin_expect(!stack, 0))
    {
        thread_cold_t *tc = THREAD_COLD(t);
        tc->stack_base = NULL;
        tc->waiting = NULL;
        tc->func = func;
        tc->func_arg = arg;
        tc->retval = func(arg);
        tc->state = FINISHED;
        *newthread = (thread_t)t;
        return 0;
    }

    thread_cold_t *tc = THREAD_COLD(t);
    tc->stack_base = stack;
    tc->state = READY;
    tc->retval = NULL;
    tc->waiting = NULL;
    tc->func = func;
    tc->func_arg = arg;
    t->rsp = stack_init(stack, func, arg);

#ifndef NVALGRIND
    tc->valgrind_stackid = VALGRIND_STACK_REGISTER(stack, (char *)stack + STACK_SIZE);
#endif

    dedup_insert(func, arg, t);

    flush_last_created();
    last_created = t;
    *newthread = (thread_t)t;
    return 0;
}

__attribute__((visibility("default"), hot, flatten)) int thread_yield(void)
{
    flush_last_created();

    if (__builtin_expect(is_sched_empty(), 0))
        return 0;

    thread_hot_t *prev = current;

    sched_enqueue(prev);

    thread_hot_t *next = sched_dequeue_fifo();
    current = next;

    context_switch(&prev->rsp, next->rsp);
    return 0;
}

__attribute__((visibility("default"), hot)) int thread_join(thread_t thread, void **retval)
{
    thread_hot_t *t = (thread_hot_t *)thread;
    thread_cold_t *tc = THREAD_COLD(t);

    if (__builtin_expect(tc->state != FINISHED, 1))
    {
        tc->waiting = current;

        thread_hot_t *next;
        if (__builtin_expect(last_created == t, 1))
        {

            next = t;
            last_created = NULL;
        }
        else
        {

            flush_last_created();
            next = sched_dequeue_lifo();
            if (__builtin_expect(!next, 0))
                return -1;
        }

        thread_hot_t *prev = current;
        current = next;

        __builtin_prefetch(next->rsp, 0, 3);
        context_switch(&prev->rsp, next->rsp);
    }

    if (retval)
        *retval = tc->retval;

    if (__builtin_expect(tc->func != NULL, 1))
    {
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
    return 0;
}

__attribute__((visibility("default"), hot)) void thread_exit(void *retval)
{
    thread_cold_t *cc = THREAD_COLD(current);
    cc->retval = retval;
    cc->state = FINISHED;

    if (__builtin_expect(cc->func != NULL, 1))
    {
        dedup_remove(cc->func, cc->func_arg);
        cc->func = NULL;
    }

    if (__builtin_expect(cc->waiting != NULL, 1))
    {
        current = cc->waiting;
        context_restore(cc->waiting->rsp);
    }

    flush_last_created();

    if (__builtin_expect(is_sched_empty(), 0))
        context_restore(exit_rsp);

    thread_hot_t *next = sched_dequeue_lifo();
    current = next;

    context_restore(next->rsp);
}

// Mutex (stubs)

__attribute__((visibility("default"))) int thread_mutex_init(thread_mutex_t *mutex)
{
    (void)mutex;
    return 0;
}
__attribute__((visibility("default"))) int thread_mutex_destroy(thread_mutex_t *mutex)
{
    (void)mutex;
    return 0;
}
__attribute__((visibility("default"))) int thread_mutex_lock(thread_mutex_t *mutex)
{
    (void)mutex;
    return 0;
}
__attribute__((visibility("default"))) int thread_mutex_unlock(thread_mutex_t *mutex)
{
    (void)mutex;
    return 0;
}
