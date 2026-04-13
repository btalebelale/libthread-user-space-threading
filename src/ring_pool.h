#ifndef __RING_POOL_H__
#define __RING_POOL_H__




#include "thread_internal.h"
#include <assert.h>

#ifdef NDEBUG
#define DBG_ASSERT(expr) do { if (!(expr)) __builtin_unreachable(); } while (0)
#else
#define DBG_ASSERT(expr) assert(expr)
#endif


#ifndef RING_BITS
#define RING_BITS   12
#endif
#define RING_CAP    (1 << RING_BITS)
#define RING_MASK   (RING_CAP - 1)


typedef struct { unsigned head; unsigned tail; } ring_idx_t;

//Pas d'encapsulation = Pas d'indirection 
extern thread_hot_t *ring_data[];
extern ring_idx_t ring_idx;

static inline __attribute__((always_inline))
void ring_init(void)
{
    ring_idx.head = 0;
    ring_idx.tail = 0;
}

static inline __attribute__((always_inline))
int ring_empty(void)
{
    return ring_idx.head == ring_idx.tail;
}

static inline __attribute__((always_inline))
void ring_put_last(thread_hot_t *th)
{
    DBG_ASSERT((ring_idx.tail - ring_idx.head) <= RING_MASK);
    ring_data[ring_idx.tail & RING_MASK] = th;
    ring_idx.tail++;
}

static inline __attribute__((always_inline))
thread_hot_t *ring_remove_first(void)
{
    thread_hot_t *th = ring_data[ring_idx.head & RING_MASK];
    ring_idx.head++;
    __builtin_prefetch(ring_data[ring_idx.head & RING_MASK], 0, 3);
    return th;
}

static inline __attribute__((always_inline))
thread_hot_t *ring_remove_last(void)
{
    ring_idx.tail--;
    return ring_data[ring_idx.tail & RING_MASK];
}

#endif /* __RING_POOL_H__ */
