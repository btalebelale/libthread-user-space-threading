#ifndef __DEDUP_H__
#define __DEDUP_H__

#include "thread_internal.h"

#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef __SSE2__
#include <emmintrin.h>
#endif

#ifndef DEDUP_BITS
#define DEDUP_BITS 12
#endif
#define DEDUP_CAP (1 << DEDUP_BITS)
#define DEDUP_MAGIC 11400714819323198485ULL

typedef void *(*func_ptr_t)(void *);

#define SWISS_GROUP_SIZE 16
#define SWISS_GROUPS (DEDUP_CAP / SWISS_GROUP_SIZE)
#define SWISS_GROUP_MASK (SWISS_GROUPS - 1)

#define CTRL_EMPTY ((int8_t)0x80)
#define CTRL_DELETED ((int8_t)0xFE)

static int8_t *swiss_ctrl;
static func_ptr_t *swiss_key_func;
static void **swiss_key_arg;
static thread_hot_t **swiss_val;


static inline __attribute__((always_inline))
uint64_t dedup_hash_full(func_ptr_t func, void *arg)
{
    uint64_t h = (uint64_t)(uintptr_t)func * DEDUP_MAGIC;
    h ^= (uint64_t)(uintptr_t)arg * 7046029254386353131ULL;
    return h;
}


static inline __attribute__((always_inline))
uint32_t swiss_group_index(uint64_t h)
{
    return (uint32_t)(h >> 50) & SWISS_GROUP_MASK;
}

static inline __attribute__((always_inline))
int8_t swiss_tag(uint64_t h)
{
    return (int8_t)((h >> 43) & 0x7F);
}



#ifdef __SSE2__

static inline __attribute__((always_inline))
uint32_t swiss_match(const int8_t *ctrl, int8_t tag)
{
    __m128i group = _mm_load_si128((const __m128i *)ctrl);
    __m128i cmp = _mm_cmpeq_epi8(group, _mm_set1_epi8(tag));
    return (uint32_t)_mm_movemask_epi8(cmp);
}

static inline __attribute__((always_inline))
uint32_t swiss_match_empty(const int8_t *ctrl)
{
    __m128i group = _mm_load_si128((const __m128i *)ctrl);
    __m128i cmp = _mm_cmpeq_epi8(group, _mm_set1_epi8(CTRL_EMPTY));
    return (uint32_t)_mm_movemask_epi8(cmp);
}

static inline __attribute__((always_inline))
uint32_t swiss_match_empty_or_deleted(const int8_t *ctrl)
{
    __m128i group = _mm_load_si128((const __m128i *)ctrl);
    return (uint32_t)_mm_movemask_epi8(group);
}

#else

static inline __attribute__((always_inline))
uint32_t swiss_match(const int8_t *ctrl, int8_t tag)
{
    uint32_t mask = 0;
    for (int i = 0; i < 16; i++)
        if (ctrl[i] == tag) mask |= (1u << i);
    return mask;
}

static inline __attribute__((always_inline))
uint32_t swiss_match_empty(const int8_t *ctrl)
{
    uint32_t mask = 0;
    for (int i = 0; i < 16; i++)
        if (ctrl[i] == CTRL_EMPTY) mask |= (1u << i);
    return mask;
}

static inline __attribute__((always_inline))
uint32_t swiss_match_empty_or_deleted(const int8_t *ctrl)
{
    uint32_t mask = 0;
    for (int i = 0; i < 16; i++)
        if (ctrl[i] < 0) mask |= (1u << i);
    return mask;
}

#endif

typedef struct { uint32_t group; uint32_t stride; } swiss_probe_t;

static inline __attribute__((always_inline))
swiss_probe_t swiss_probe_start(uint64_t h)
{
    return (swiss_probe_t){ swiss_group_index(h), 0 };
}

static inline __attribute__((always_inline))
void swiss_probe_next(swiss_probe_t *p)
{
    p->stride++;
    p->group = (p->group + p->stride) & SWISS_GROUP_MASK;
}


static inline __attribute__((always_inline))
thread_hot_t *dedup_lookup(func_ptr_t func, void *arg)
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

static inline __attribute__((always_inline))
void dedup_insert(func_ptr_t func, void *arg, thread_hot_t *t)
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

static inline
void dedup_remove(func_ptr_t func, void *arg)
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

__attribute__((cold))
static void dedup_mem_init(void)
{
    // Single contiguous allocation for better cache locality
    size_t ctrl_sz = (size_t)DEDUP_CAP;
    size_t func_sz = (size_t)DEDUP_CAP * sizeof(func_ptr_t);
    size_t arg_sz  = (size_t)DEDUP_CAP * sizeof(void *);
    size_t val_sz  = (size_t)DEDUP_CAP * sizeof(thread_hot_t *);
    // Align each sub-array to 64 bytes (cache line)
    ctrl_sz = (ctrl_sz + 63) & ~(size_t)63;
    func_sz = (func_sz + 63) & ~(size_t)63;
    arg_sz  = (arg_sz + 63) & ~(size_t)63;
    size_t total = ctrl_sz + func_sz + arg_sz + val_sz;

    char *mem = mmap(NULL, total,
                     PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (__builtin_expect(mem == MAP_FAILED, 0))
    {
        perror("mmap dedup swiss");
        exit(1);
    }
    swiss_ctrl     = (int8_t *)mem;
    swiss_key_func = (func_ptr_t *)(mem + ctrl_sz);
    swiss_key_arg  = (void **)(mem + ctrl_sz + func_sz);
    swiss_val      = (thread_hot_t **)(mem + ctrl_sz + func_sz + arg_sz);
    memset(swiss_ctrl, CTRL_EMPTY, DEDUP_CAP);
}

__attribute__((cold))
static void dedup_mem_destroy(void)
{
    if (swiss_ctrl && swiss_ctrl != MAP_FAILED)
    {
        size_t ctrl_sz = ((size_t)DEDUP_CAP + 63) & ~(size_t)63;
        size_t func_sz = ((size_t)DEDUP_CAP * sizeof(func_ptr_t) + 63) & ~(size_t)63;
        size_t arg_sz  = ((size_t)DEDUP_CAP * sizeof(void *) + 63) & ~(size_t)63;
        size_t val_sz  = (size_t)DEDUP_CAP * sizeof(thread_hot_t *);
        munmap(swiss_ctrl, ctrl_sz + func_sz + arg_sz + val_sz);
    }
    swiss_ctrl = NULL;
    swiss_key_func = NULL;
    swiss_key_arg = NULL;
    swiss_val = NULL;
}

#endif
