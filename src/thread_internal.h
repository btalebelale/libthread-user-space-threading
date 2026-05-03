#ifndef __THREAD_INTERNAL_H__
#define __THREAD_INTERNAL_H__

#include "thread.h"
#include "portable.h"
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#ifdef MULTICORE
#include <pthread.h>
#endif

// Thread states
typedef enum
{
    READY,
    FINISHED
} state_t;

// HOT : touché à chaque yield
typedef struct thread_hot
{
    void *rsp;
    struct thread_hot *sched_next;
    struct thread_hot *sched_prev;
    int priority;
} thread_hot_t;

// COLD : touché uniquement à create/join/exit
typedef struct thread_cold
{
    void *retval;
    void *stack_base;
    struct thread_hot *waiting;

    struct thread_cold *parent;
    uint8_t rank;

    uint32_t state;
    int valgrind_stackid;
    void *(*func)(void *); // pour déduplication
    void *func_arg;        // pour déduplication
    jmp_buf *inline_jmpbuf;
    uint16_t refcount;
    uint8_t started;
    thread_sigset_t pending_sigs;  // bitmask: signaux reçus, pas encore consommés
    thread_sigset_t wait_mask;     // bitmask: signaux attendus par thread_sigwait
    uint8_t sig_waiting;           // 1 si le thread est bloqué dans thread_sigwait
    int     received_sig;          // numéro du signal qui l'a réveillé
#ifdef MULTICORE
    port_spin_t cold_lock;
#endif
} thread_cold_t;

#ifndef MULTICORE
extern thread_hot_t *current;
#endif

/* Reset des champs d'un descripteur cold à leur valeur "fraîche". Utilisé
 * par thread_create (recyclage via free-list) et init_system (main thread).
 * Ne touche PAS cold_lock (MN) : la spinlock est initialisée une seule fois
 * à l'allocation du slot dans le slab et reste valide entre les recyclages.
 * Ne touche pas non plus thread_hot_t (rsp, sched_*, priority) — c'est au
 * caller de les poser. */
static inline void thread_cold_reset(thread_cold_t *tc)
{
    tc->retval        = NULL;
    tc->stack_base    = NULL;
    tc->waiting       = NULL;
    tc->parent        = tc;
    tc->rank          = 0;
    tc->state         = READY;
    tc->func          = NULL;
    tc->func_arg      = NULL;
    tc->inline_jmpbuf = NULL;
    tc->refcount      = 1;
    tc->started       = 0;
    tc->pending_sigs  = 0;
    tc->wait_mask     = 0;
    tc->sig_waiting   = 0;
    tc->received_sig  = 0;
}

/* context_switch / context_restore / thread_trampoline déclarés dans portable.h
 * (asm sur fast path, wrappers ucontext sur fallback). */

/* Slabs hot/cold + alloc paresseuse de pile : définis dans thread.c (mono)
 * ou thread_mn.c (MN), exposés hidden pour que thread_sync.c et
 * thread_common.h puissent les référencer (THREAD_COLD utilise hot_slab et
 * cold_slab dans son cast). */
__attribute__((visibility("hidden"))) extern thread_hot_t  *hot_slab;
__attribute__((visibility("hidden"))) extern thread_cold_t *cold_slab;
__attribute__((visibility("hidden"))) extern void lazy_stack_alloc(thread_hot_t *t);


typedef struct mutex_internal
{
    int locked;
    thread_hot_t *wait_head;  // premier thread en attente
    thread_hot_t *wait_tail;  // dernier thread en attente
#ifdef MULTICORE
    port_spin_t lock;
#endif
} mutex_internal_t;


typedef struct sem_internal {
    int              count;      // valeur courante du sémaphore
    thread_hot_t    *wait_head;  // FIFO des threads bloqués
    thread_hot_t    *wait_tail;
#ifdef MULTICORE
    port_spin_t lock;
#endif
} sem_internal_t;

#ifdef MULTICORE
/* Disposition cache-line (1 worker = 1 cache line hot + 1 cold) :
 *   ligne 1 (64B)  : tous les champs hot/warm — touchés sur le hot path
 *                    schedule + thread_create (current, sched_rsp,
 *                    after_switch, after_arg, inline_executing, id, et
 *                    last_created*). Le padding était auparavant gaspillé,
 *                    on l'occupe avec last_created* qui faisaient une 2e
 *                    cache miss à chaque thread_create + schedule combinés.
 *   cold           : sched_stack_base, pthread, is_main (init / shutdown).
 *
 * `aligned(64)` + tableau global aligné => aucun worker[i] ne partage
 * de cache line avec worker[i±1]. */
typedef struct worker
{
    /* --- Ligne 1 : hot scheduler + warm thread_create (saturée à 64B) --- */
    thread_hot_t *current;                 /* 8 */
    void *sched_rsp;                       /* 8 */
    void (*after_switch)(void *);          /* 8 */
    void *after_arg;                       /* 8 */
    int   inline_executing;                /* 4 */
    int   id;                              /* 4 */
    thread_hot_t *last_created;            /* 8 */
    void *(*last_created_func)(void *);    /* 8 */
    void *last_created_arg;                /* 8 — total 64B */

    /* --- Cold --- */
    void *sched_stack_base;
    pthread_t pthread;
    int  is_main;
} __attribute__((aligned(64))) worker_t;

extern __thread worker_t *self_worker;
#endif /* MULTICORE */

#endif
