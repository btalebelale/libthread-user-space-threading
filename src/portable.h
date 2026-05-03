/* portable.h — abstraction des dépendances arch/OS.
 *
 * Modèle "fast path / fallback" :
 *   - PORT_FAST_PATH = 1  → x86_64 + GCC + Linux/glibc
 *       Tout le code optimisé : asm context_switch, MAP_POPULATE / MAP_STACK,
 *       pthread_spinlock_t, pthread_setaffinity_np, MADV_HUGEPAGE.
 *   - PORT_FAST_PATH = 0  → tout le reste (autres archis, macOS, BSD…)
 *       Implémentations génériques POSIX. *Aucune* garantie de perf : le but
 *       est juste que la lib compile et fonctionne. Le hot path nanoseconde
 *       est un objectif x86_64 uniquement (cf. CLAUDE.md).
 *
 * Les sites d'utilisation dans le reste du code n'ont plus *aucun* #ifdef
 * lié à l'archi : ils appellent PORT_SPIN_LOCK, PORT_PIN_THIS, etc.
 */
#ifndef __PORTABLE_H__
#define __PORTABLE_H__

#include <pthread.h>
#include <sys/mman.h>

/* ---------- 1. Détection de l'environnement cible ----------
 * PORT_FORCE_FALLBACK (posé par le Makefile en mode fallback) court-circuite
 * la détection pour permettre de tester le chemin générique sur une machine
 * x86_64+Linux. */
#if defined(__x86_64__) && defined(__GNUC__) && defined(__linux__) && !defined(PORT_FORCE_FALLBACK)
  #define PORT_FAST_PATH 1
#else
  #define PORT_FAST_PATH 0
#endif

/* ---------- 2. Spinlock ----------
 * Sur fast path : pthread_spinlock_t (busy-wait, ~10 ns sous contention).
 * Sur fallback  : pthread_mutex_t (3-5× plus lent ; absent de macOS sinon). */
#if PORT_FAST_PATH
  typedef pthread_spinlock_t port_spin_t;
  #define PORT_SPIN_INIT(s)    pthread_spin_init((s), PTHREAD_PROCESS_PRIVATE)
  #define PORT_SPIN_LOCK(s)    pthread_spin_lock(s)
  #define PORT_SPIN_UNLOCK(s)  pthread_spin_unlock(s)
  #define PORT_SPIN_DESTROY(s) pthread_spin_destroy(s)
#else
  typedef pthread_mutex_t port_spin_t;
  #define PORT_SPIN_INIT(s)    pthread_mutex_init((s), NULL)
  #define PORT_SPIN_LOCK(s)    pthread_mutex_lock(s)
  #define PORT_SPIN_UNLOCK(s)  pthread_mutex_unlock(s)
  #define PORT_SPIN_DESTROY(s) pthread_mutex_destroy(s)
#endif

/* ---------- 3. Flags mmap ----------
 * MAP_STACK   : hint kernel "cette plage va servir de pile" (Linux only).
 * MAP_POPULATE: pré-fault les pages (Linux only). */
#if PORT_FAST_PATH
  #define PORT_MMAP_STACK_FLAGS    (MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK)
  #define PORT_MMAP_POPULATE_FLAG  MAP_POPULATE
#else
  #define PORT_MMAP_STACK_FLAGS    (MAP_PRIVATE | MAP_ANONYMOUS)
  #define PORT_MMAP_POPULATE_FLAG  0
#endif

/* ---------- 4. madvise hugepage ---------- */
#if PORT_FAST_PATH
  #include <sys/mman.h>
  #define PORT_MADV_HUGEPAGE(addr, sz) ((void)madvise((addr), (sz), MADV_HUGEPAGE))
#else
  #define PORT_MADV_HUGEPAGE(addr, sz) ((void)(addr), (void)(sz))
#endif

/* ---------- 5. Pin CPU ----------
 * pthread_setaffinity_np = extension GNU. macOS: thread_policy_set (différent)
 * — on no-op proprement, le scheduler kernel se débrouille. */
#if PORT_FAST_PATH
  #include <sched.h>
  #define PORT_PIN_THIS(cpu) do {                                              \
      cpu_set_t _set; CPU_ZERO(&_set); CPU_SET((cpu), &_set);                  \
      pthread_setaffinity_np(pthread_self(), sizeof(_set), &_set);             \
  } while (0)
#else
  #define PORT_PIN_THIS(cpu) ((void)(cpu))
#endif

/* ---------- 6. Context switch ----------
 * Fast path : asm hand-tuned dans context_switch.S (push/pop des callee-saved
 *             x86_64 + bascule de %rsp). ~5-15 ns.
 * Fallback  : wrappers C autour de swapcontext / setcontext (~250-400 ns).
 *
 * Signatures identiques dans les deux modes : le reste du code ne voit pas
 * la différence. Le contenu de `void *rsp` change cependant :
 *   - fast path : pointeur dans la pile du thread (en haut des registres
 *                 callee-saved poussés par stack_init / par l'asm).
 *   - fallback  : pointeur vers un ucontext_t (alloué juste après la guard
 *                 page de la pile, voir port_make_ctx). */
__attribute__((visibility("hidden"))) extern void context_switch(void **old_rsp, void *new_rsp);
__attribute__((visibility("hidden"), __noreturn__)) extern void context_restore(void *new_rsp);

#if PORT_FAST_PATH
__attribute__((visibility("hidden"))) extern void thread_trampoline(void);
#endif

/* Fallback : prépare un ucontext_t logé dans `stack_base` (juste après la
 * guard page) et l'arme pour appeler thread_entry(func, arg). Renvoie le
 * pointeur à stocker dans `t->rsp`. Inutilisé en fast path (qui pose
 * directement les registres sur la pile via stack_init). */
#if !PORT_FAST_PATH
__attribute__((visibility("hidden"))) void *port_make_ctx(void *stack_base,
                                                          void *(*func)(void *),
                                                          void *arg);
/* Pour le worker M:N : alloue un ucontext_t initial qui démarre `entry()`.
 * Utilisé pour la sched_stack du worker 0. */
__attribute__((visibility("hidden"))) void *port_make_sched_ctx(void *stack_base,
                                                                void (*entry)(void));
/* Variante pour piles arbitraires (exit_stack du mono : 4 Kio fixe, pas
 * de guard page, taille passée explicitement). */
__attribute__((visibility("hidden"))) void *port_make_simple_ctx(void *stack_base,
                                                                 size_t stack_size,
                                                                 void (*entry)(void));
/* Pour le main thread (mono ou MN worker 0) : ucontext_t "vide" servant de
 * cible de sauvegarde au premier swapcontext. Le storage est statique
 * interne. */
__attribute__((visibility("hidden"))) void *port_main_ctx(void);
#endif

#endif /* __PORTABLE_H__ */
