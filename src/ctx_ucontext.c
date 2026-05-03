/* ctx_ucontext.c — fallback générique POSIX pour le context switch.
 *
 * Compilé UNIQUEMENT quand PORT_FAST_PATH = 0 (le Makefile choisit entre ce
 * fichier et context_switch.S selon `uname -m`). Sur x86_64+Linux+GCC, c'est
 * l'asm qui prend la main et ce fichier n'existe pas dans le build.
 *
 * Performance : ~250-400 ns par swapcontext (kernel rt_sigprocmask à chaque
 * coup), contre ~5-15 ns pour l'asm. Acceptable pour porter la lib sur ARM /
 * macOS / FreeBSD ; pas pour le bench.
 *
 * Convention de stockage de `void *rsp` :
 *   - fast path  (asm) : pointeur dans la pile utilisateur du thread.
 *   - fallback (ici)   : pointeur vers un ucontext_t logé juste après la
 *                        page de garde de la pile (cf. port_make_ctx). */

#define _XOPEN_SOURCE 600

#include "portable.h"

#if !PORT_FAST_PATH

#include "thread_internal.h"
#include "thread_common.h"

#include <ucontext.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- Symboles définis dans thread.c (mode mono) ----------
 * En MN ils sont remplacés par self_worker->{...} et thread_yield_asm n'est
 * pas appelé (worker_loop appelle context_switch directement). */
#ifndef MULTICORE
extern thread_hot_t *current;
extern thread_hot_t *last_created;
extern int inline_executing;
__attribute__((visibility("hidden"))) extern void flush_last_created_slow(void);
__attribute__((visibility("hidden"))) extern void thread_entry(void *(*func)(void *), void *arg);
#else
__attribute__((visibility("hidden"))) extern void thread_entry(void *(*func)(void *), void *arg);
#endif

/* ---------- context_switch / context_restore ---------- */

__attribute__((visibility("hidden")))
void context_switch(void **old_rsp, void *new_rsp)
{
    ucontext_t *old = (ucontext_t *)*old_rsp;
    ucontext_t *neu = (ucontext_t *)new_rsp;
    if (old == NULL) {
        /* Premier switch d'un thread "natif" (main user thread, ou pthread
         * worker en MN) qui n'a pas encore de ucontext_t : on lui en attribue
         * un en TLS. Voir port_main_ctx ci-dessous pour les caveats MN. */
        old = (ucontext_t *)port_main_ctx();
        *old_rsp = old;
    }
    if (swapcontext(old, neu) != 0) {
        perror("swapcontext");
        abort();
    }
}

__attribute__((visibility("hidden"), __noreturn__))
void context_restore(void *new_rsp)
{
    setcontext((ucontext_t *)new_rsp);
    /* setcontext ne retourne jamais en cas de succès. */
    perror("setcontext");
    abort();
}

/* ---------- Trampoline ----------
 * makecontext ne sait passer que des `int` variadiques. Pour transmettre
 * func+arg (deux pointeurs 64 bits) on les splitte en 4 unsigned 32 bits. */
static void uctx_trampoline(unsigned f_hi, unsigned f_lo,
                            unsigned a_hi, unsigned a_lo)
{
    uintptr_t fp = ((uintptr_t)f_hi << 32) | (uintptr_t)f_lo;
    uintptr_t ap = ((uintptr_t)a_hi << 32) | (uintptr_t)a_lo;
    thread_entry((void *(*)(void *))fp, (void *)ap);
}

/* ---------- port_make_ctx : ucontext pour un thread utilisateur ---------- */
__attribute__((visibility("hidden")))
void *port_make_ctx(void *stack_base, void *(*func)(void *), void *arg)
{
    /* Layout de la pile en fallback :
     *   [0 .. STACK_GUARD)                         page de garde (PROT_NONE)
     *   [STACK_GUARD .. STACK_GUARD+sizeof(ctx))   ucontext_t du thread
     *   [aligné 16  .. STACK_SIZE - 8)             pile utilisable
     *   [STACK_SIZE - 8 .. STACK_SIZE)             free-list pointer */
    ucontext_t *ctx = (ucontext_t *)((char *)stack_base + STACK_GUARD);
    if (getcontext(ctx) != 0) { perror("getcontext"); abort(); }

    char *ss_base = (char *)ctx + sizeof(ucontext_t);
    uintptr_t a = ((uintptr_t)ss_base + 15u) & ~(uintptr_t)15u;
    ss_base = (char *)a;
    size_t ss_size = (size_t)STACK_SIZE
                     - (size_t)((char *)ss_base - (char *)stack_base)
                     - sizeof(void *);
    ctx->uc_stack.ss_sp    = ss_base;
    ctx->uc_stack.ss_size  = ss_size;
    ctx->uc_stack.ss_flags = 0;
    ctx->uc_link = NULL;

    uintptr_t fp = (uintptr_t)func;
    uintptr_t ap = (uintptr_t)arg;
    makecontext(ctx, (void (*)(void))uctx_trampoline, 4,
                (unsigned)((fp >> 32) & 0xFFFFFFFFu),
                (unsigned)( fp        & 0xFFFFFFFFu),
                (unsigned)((ap >> 32) & 0xFFFFFFFFu),
                (unsigned)( ap        & 0xFFFFFFFFu));
    return ctx;
}

/* ---------- port_make_simple_ctx : ucontext sur pile fournie, sans guard ---------- */
__attribute__((visibility("hidden")))
void *port_make_simple_ctx(void *stack_base, size_t stack_size, void (*entry)(void))
{
    /* Le ucontext_t partage la pile : on le pose au début et on lui donne
     * la suite comme uc_stack. Convient aux piles fixes type exit_stack. */
    ucontext_t *ctx = (ucontext_t *)stack_base;
    if (getcontext(ctx) != 0) { perror("getcontext"); abort(); }
    char *ss_base = (char *)ctx + sizeof(ucontext_t);
    uintptr_t a = ((uintptr_t)ss_base + 15u) & ~(uintptr_t)15u;
    ss_base = (char *)a;
    size_t ss_size = stack_size - (size_t)((char *)ss_base - (char *)stack_base);
    ctx->uc_stack.ss_sp    = ss_base;
    ctx->uc_stack.ss_size  = ss_size;
    ctx->uc_stack.ss_flags = 0;
    ctx->uc_link = NULL;
    makecontext(ctx, entry, 0);
    return ctx;
}

/* ---------- port_make_sched_ctx : ucontext pour la sched_stack worker 0 ---------- */
__attribute__((visibility("hidden")))
void *port_make_sched_ctx(void *stack_base, void (*entry)(void))
{
    ucontext_t *ctx = (ucontext_t *)((char *)stack_base + STACK_GUARD);
    if (getcontext(ctx) != 0) { perror("getcontext"); abort(); }

    char *ss_base = (char *)ctx + sizeof(ucontext_t);
    uintptr_t a = ((uintptr_t)ss_base + 15u) & ~(uintptr_t)15u;
    ss_base = (char *)a;
    size_t ss_size = (size_t)SCHED_STACK_SIZE
                     - (size_t)((char *)ss_base - (char *)stack_base)
                     - sizeof(void *);
    ctx->uc_stack.ss_sp    = ss_base;
    ctx->uc_stack.ss_size  = ss_size;
    ctx->uc_stack.ss_flags = 0;
    ctx->uc_link = NULL;
    makecontext(ctx, entry, 0);
    return ctx;
}

/* ---------- port_main_ctx ----------
 * Storage TLS pour les threads "natifs" (= déjà sur leur propre pile
 * système : main, ou pthread worker en MN). Au premier swapcontext, le
 * thread atterrit ici comme cible de sauvegarde.
 *
 * Caveat MN : si le main user thread se fait voler par un autre worker
 * APRÈS son premier switch, son rsp pointera toujours vers le storage TLS
 * du worker d'origine. Race de principe ; jamais déclenchée par les tests
 * actuels mais à garder en tête. */
static __thread ucontext_t main_ctx_storage;

__attribute__((visibility("hidden")))
void *port_main_ctx(void)
{
    return &main_ctx_storage;
}

/* ---------- thread_yield_asm fallback (mono) ----------
 * Réplique exactement la logique du label thread_yield_asm dans
 * context_switch.S. Tous les chemins testés (yield seul, yield avec
 * last_created en attente, yield avec next->rsp == NULL → lazy alloc). */
#ifndef MULTICORE
__attribute__((visibility("hidden"))) extern void lazy_stack_alloc(thread_hot_t *t);

__attribute__((visibility("hidden")))
int thread_yield_asm(void)
{
    if (inline_executing) return 0;
    if (last_created) flush_last_created_slow();

    thread_hot_t *prev = current;
    thread_hot_t *next = prev->sched_next;
    if (prev == next) return 0;

    if (next->rsp == NULL) lazy_stack_alloc(next);

    current = next;
    context_switch(&prev->rsp, next->rsp);
    return 0;
}

/* Sur fast path mono sans USE_PRIORITY/USE_PREEMPTION, l'asm posait l'alias
 * `.set thread_yield, thread_yield_asm`. En fallback, on fournit thread_yield
 * en C ici (mêmes contraintes : un seul define dans toute la lib). */
#if !defined(USE_PRIORITY) && !defined(USE_PREEMPTION)
__attribute__((visibility("default")))
int thread_yield(void)
{
    return thread_yield_asm();
}
#endif
#endif /* !MULTICORE */

#endif /* !PORT_FAST_PATH */
