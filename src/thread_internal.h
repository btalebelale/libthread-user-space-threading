#ifndef __THREAD_INTERNAL_H__
#define __THREAD_INTERNAL_H__

#include "thread.h"
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#ifdef MULTICORE
#include <pthread.h>
#endif

typedef enum
{
    READY,
    FINISHED
} state_t;

typedef struct thread_hot
{
    void *rsp;                        /* 0  */
    struct thread_hot *sched_next;    /* 8  */
    void *saved_rbx;                  /* 16 */
    void *saved_rbp;                  /* 24 */
    void *saved_r12;                  /* 32 */
    void *saved_r13;                  /* 40 */
    void *saved_r14;                  /* 48 */
    void *saved_r15;                  /* 56 */
} thread_hot_t;
_Static_assert(sizeof(thread_hot_t) == 64, "thread_hot_t must be one cache line");
_Static_assert(__builtin_offsetof(thread_hot_t, rsp) == 0,         "rsp@0");
_Static_assert(__builtin_offsetof(thread_hot_t, sched_next) == 8,  "sched_next@8");
_Static_assert(__builtin_offsetof(thread_hot_t, saved_rbx) == 16,  "saved_rbx@16");
_Static_assert(__builtin_offsetof(thread_hot_t, saved_r15) == 56,  "saved_r15@56");

typedef struct thread_cold
{
    void *retval;
    void *stack_base;
    struct thread_hot *waiting;
    struct thread_hot *sched_prev;
    int priority;

    struct thread_cold *parent;
    uint8_t rank;

    uint32_t state;
    int valgrind_stackid;
    void *(*func)(void *);
    void *func_arg;
    jmp_buf *inline_jmpbuf;
    uint16_t refcount;
    uint8_t started;
    thread_sigset_t pending_sigs;
    thread_sigset_t wait_mask;
    uint8_t sig_waiting;
    int     received_sig;
#ifdef MULTICORE
    pthread_spinlock_t cold_lock;
#endif
} thread_cold_t;

#ifndef MULTICORE
extern thread_hot_t *current;
#endif

static inline void thread_cold_reset(thread_cold_t *target_cold)
{
    target_cold->retval        = NULL;
    target_cold->stack_base    = NULL;
    target_cold->waiting       = NULL;
    target_cold->parent        = target_cold;
    target_cold->rank          = 0;
    target_cold->state         = READY;
    target_cold->func          = NULL;
    target_cold->func_arg      = NULL;
    target_cold->inline_jmpbuf = NULL;
    target_cold->refcount      = 1;
    target_cold->started       = 0;
    target_cold->pending_sigs  = 0;
    target_cold->wait_mask     = 0;
    target_cold->sig_waiting   = 0;
    target_cold->received_sig  = 0;
}

__attribute__((visibility("hidden"))) extern void context_switch(thread_hot_t *prev, thread_hot_t *next);
__attribute__((visibility("hidden"), __noreturn__)) extern void context_restore(thread_hot_t *next);

#ifdef MULTICORE
__attribute__((visibility("hidden"))) extern void context_switch_raw(void **old_rsp, void *new_rsp);
__attribute__((visibility("hidden"), __noreturn__)) extern void context_restore_raw(void *new_rsp);
#endif
__attribute__((visibility("hidden"))) extern void thread_trampoline(void);

__attribute__((visibility("hidden"))) extern thread_hot_t  *hot_slab;
__attribute__((visibility("hidden"))) extern thread_cold_t *cold_slab;
__attribute__((visibility("hidden"))) extern void lazy_stack_alloc(thread_hot_t *t);


typedef struct mutex_internal
{
    int locked;
    thread_hot_t *wait_head;
    thread_hot_t *wait_tail;
#ifdef MULTICORE
    pthread_spinlock_t lock;
#endif
} mutex_internal_t;


typedef struct sem_internal {
    int              count;
    thread_hot_t    *wait_head;
    thread_hot_t    *wait_tail;
#ifdef MULTICORE
    pthread_spinlock_t lock;
#endif
} sem_internal_t;

#ifdef MULTICORE
typedef struct worker
{
    thread_hot_t *current;                 /* 8 */
    void *sched_rsp;                       /* 8 */
    void (*after_switch)(void *);          /* 8 */
    void *after_arg;                       /* 8 */
    int   inline_executing;                /* 4 */
    int   id;                              /* 4 */
    thread_hot_t *last_created;            /* 8 */
    void *(*last_created_func)(void *);    /* 8 */
    void *last_created_arg;                /* 8 */

    /* Préemption MN : 1 quand le worker exécute du code sur la sched-stack
     * (worker_loop, callbacks after_switch, schedule_*). Le handler SIGALRM
     * vérifie ce flag avant de yielder — un signal qui frappe entre le moment
     * où on entre dans schedule() et le retour côté nouveau thread doit être
     * ignoré, sinon on context_switch_raw au-dessus d'un état partiellement
     * sauvegardé (cf crash 107 sans cette protection). */
    volatile int on_sched_stack;

    void *sched_stack_base;
    pthread_t pthread;
    int  is_main;
} __attribute__((aligned(64))) worker_t;

extern __thread worker_t *self_worker;
#endif

#endif
