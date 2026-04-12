#ifndef __RING_POOL_H__
#define __RING_POOL_H__

// Ring buffer FIFO statique pour le scheduler hot path.
// Pas d'allocation heap, pas d'indirection pointeur.
// Le compilateur connait les adresses a la compilation → meilleur codegen.

#include "thread_internal.h"
#include <assert.h>

#ifdef NDEBUG
#define DBG_ASSERT(expr) do { if (!(expr)) __builtin_unreachable(); } while (0)
#else
#define DBG_ASSERT(expr) assert(expr)
#endif

// Capacite fixe, puissance de 2
#define RING_CAP    (1 << 16)
#define RING_MASK   (RING_CAP - 1)

// Definis dans scheduler.c — une seule instance partagee
extern thread_hot_t *ring_data[];
extern unsigned ring_head;
extern unsigned ring_tail;

static inline __attribute__((always_inline))
void ring_init(void)
{
    ring_head = 0;
    ring_tail = 0;
}

static inline __attribute__((always_inline))
int ring_empty(void)
{
    return ring_head == ring_tail;
}

static inline __attribute__((always_inline))
void ring_put_last(thread_hot_t *th)
{
    DBG_ASSERT((ring_tail - ring_head) <= RING_MASK);
    ring_data[ring_tail & RING_MASK] = th;
    ring_tail++;
}

static inline __attribute__((always_inline))
thread_hot_t *ring_remove_first(void)
{
    thread_hot_t *th = ring_data[ring_head & RING_MASK];
    ring_head++;
    // Prefetch le thread d'apres : donne 1 cycle complet de yield
    // pour que le prefetch atteigne L1 (vs juste avant context_switch = trop tard)
    thread_hot_t *peek = ring_data[ring_head & RING_MASK];
    if (__builtin_expect(peek != NULL, 1))
        __builtin_prefetch(peek->rsp, 0, 3);
    return th;
}

static inline __attribute__((always_inline))
thread_hot_t *ring_remove_last(void)
{
    ring_tail--;
    return ring_data[ring_tail & RING_MASK];
}

#endif /* __RING_POOL_H__ */
