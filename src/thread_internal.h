#ifndef __THREAD_INTERNAL_H__
#define __THREAD_INTERNAL_H__

#include "thread.h"
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
} thread_hot_t;

// COLD : touché uniquement à create/join/exit
typedef struct thread_cold
{
    void *retval;
    void *stack_base;
    struct thread_hot *waiting;

    struct thread_cold *daddy;
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
    pthread_spinlock_t cold_lock;
#endif
} thread_cold_t;

#ifndef MULTICORE
extern thread_hot_t *current;
#endif

__attribute__((visibility("hidden"))) extern void context_switch(void **old_rsp, void *new_rsp);
__attribute__((visibility("hidden"), __noreturn__)) extern void context_restore(void *new_rsp);
__attribute__((visibility("hidden"))) extern void thread_trampoline(void);

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
    pthread_spinlock_t lock;
#endif
} mutex_internal_t;


typedef struct sem_internal {
    int              count;      // valeur courante du sémaphore
    thread_hot_t    *wait_head;  // FIFO des threads bloqués
    thread_hot_t    *wait_tail;
#ifdef MULTICORE
    pthread_spinlock_t lock;
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
