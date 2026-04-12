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

// Capacite fixe, puissance de 2 — configurable via RING_BITS (defaut: 12 = 4096 = 32KB, tient dans L1)
#ifndef RING_BITS
#define RING_BITS   12
#endif
#define RING_CAP    (1 << RING_BITS)
#define RING_MASK   (RING_CAP - 1)

// head et tail sur la meme cache line (mono-thread cooperatif = pas de false sharing)
typedef struct { unsigned head; unsigned tail; } ring_idx_t;

// Definis dans scheduler.c — une seule instance partagee
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
    // Prefetch le thread d'apres : donne 1 cycle complet de yield
    // pour que le prefetch atteigne L1 (vs juste avant context_switch = trop tard)
    // Pas de check NULL : prefetch sur adresse invalide = NOP silencieux sur x86
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
