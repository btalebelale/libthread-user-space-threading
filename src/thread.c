#include "thread.h"
#include "scheduler.h"

#include <stdio.h>
#include <stdlib.h>
#include <valgrind/valgrind.h>
#include <sys/time.h> 
#include <signal.h>


#define STACK_SIZE  (64 * 1024)

#define STACK_CAP   16384

#define THREAD_CAP  262144


static void *stack_arena;
static int   stack_arena_next;

static void *stack_free_head;


static thread_hot_t  *hot_slab;
static thread_cold_t *cold_slab;
static int            thread_slab_next;

static thread_hot_t  *thread_free_head;


#define THREAD_COLD(hot) (&cold_slab[(hot) - hot_slab])

static void restore_sigmask(sigset_t *old)                                                                                            
{
    stack_arena = mmap(NULL, (size_t)STACK_CAP * STACK_SIZE,
                       PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (__builtin_expect(stack_arena == MAP_FAILED, 0))
    {
        perror("mmap stack arena");
        exit(1);
    }

    hot_slab = mmap(NULL, (size_t)THREAD_CAP * sizeof(thread_hot_t),
                    PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (__builtin_expect(hot_slab == MAP_FAILED, 0))
    {
        perror("mmap hot slab");
        exit(1);
    }

    cold_slab = mmap(NULL, (size_t)THREAD_CAP * sizeof(thread_cold_t),
                     PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (__builtin_expect(cold_slab == MAP_FAILED, 0))
    {
        perror("mmap cold slab");
        exit(1);
    }

static void preempt_init(void)              
{                                       
  struct sigaction sa;
  sa.sa_handler = preempt_handler;                                                                                                  
  sa.sa_flags = SA_RESTART | SA_NODEFER;
  sigemptyset(&sa.sa_mask);                                                                                                         
  sigaction(SIGALRM, &sa, NULL);        
                                      
  struct itimerval timer;
  timer.it_value.tv_sec = 0;                                                                                                        
  timer.it_value.tv_usec = PREEMPT_INTERVAL;
  timer.it_interval = timer.it_value;                                                                                               
  setitimer(ITIMER_REAL, &timer, NULL);
}

__attribute__((cold))
static void mem_destroy(void)
{
    if (stack_arena && stack_arena != MAP_FAILED)
        munmap(stack_arena, (size_t)STACK_CAP * STACK_SIZE);
    if (hot_slab && hot_slab != MAP_FAILED)
        munmap(hot_slab, (size_t)THREAD_CAP * sizeof(thread_hot_t));
    if (cold_slab && cold_slab != MAP_FAILED)
        munmap(cold_slab, (size_t)THREAD_CAP * sizeof(thread_cold_t));
    stack_arena = NULL;
    hot_slab = NULL;
    cold_slab = NULL;
}

static void *stack_alloc(void)
{
    if (__builtin_expect(stack_free_head != NULL, 1))
    {
        void *s = stack_free_head;
        stack_free_head = *(void **)s;
        return s;
    }
    if (__builtin_expect(stack_arena_next >= STACK_CAP, 0))
        return NULL;

    return (char *)stack_arena + ((size_t)stack_arena_next++ * STACK_SIZE);
}

static void stack_release(void *s)
{
    *(void **)s = stack_free_head;
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

// Stack dédié pour clean_exit
static char exit_stack[4096] __attribute__((aligned(16)));
// Frame pré-calculé pour clean_exit (évite de recalculer à chaque thread_exit)
static void *exit_rsp;

// Point d'entrée C pour les nouveaux threads, appelé depuis thread_trampoline (asm)
__attribute__((visibility("hidden")))
void thread_entry(void *(*func)(void *), void *arg)
{
    void *ret = func(arg);
    thread_exit(ret);
    __builtin_unreachable();
}

// Initialise le stack d'un nouveau thread pour context_switch.
static void *stack_init(void *stack_base, void *(*func)(void *), void *arg)
{
    uintptr_t sp = (uintptr_t)stack_base + STACK_SIZE;
    sp &= ~(uintptr_t)0xF; // alignement 16 bytes

    void **s = (void **)sp;

    // Adresse de retour pour context_switch → ret → thread_trampoline
    *(--s) = (void *)thread_trampoline;

   
    *(--s) = 0;             // rbx
    *(--s) = 0;             // rbp
    *(--s) = (void *)func;  // r12 = func
    *(--s) = arg;           // r13 = arg
    *(--s) = 0;             // r14
    *(--s) = 0;             // r15

    return s;
}

// Cycle de vie du système de threads

__attribute__((cold))
static void clean_exit(void)
{
    VALGRIND_STACK_DEREGISTER(current->valgrind_stackid);
    free(current->stack);
    free(current);
    current = NULL;
    exit(0);
}

/*
Appelée automatiquement à la fin du programme (grâce à atexit).
Libère tous les threads restants notamment le thread main.
Utilisé pour nettoyer le thread main à la fin
*/
static void thread_system_destroy(void)
{
    while (!is_sched_empty())
    {
        thread_hot_t *t = sched_dequeue_fifo();
        thread_cold_t *tc = THREAD_COLD(t);
        if (tc->stack_base)
            VALGRIND_STACK_DEREGISTER(tc->valgrind_stackid);
    }
}


/*
Initialise le système de threads (crée un thread pour le main)
Appelle atexit pour nettoyer le système à la fin en appelant
la fonction "thread_system_destroy"
*/
static void init_system(void)
{
    sched_init();

    atexit(thread_system_destroy);

    current = malloc(sizeof(thread_m));
    if (!current)
    {
        perror("malloc main thread");
        exit(1);
    }
    _setjmp(current->env);
    current->state = RUNNING;
    current->stack_size = 0;
    current->stack = NULL;
    current->retval = NULL;
    current->waiting = NULL;

    #ifdef USE_PREEMPTION                       
      preempt_init();                                                                                                                   
    #endif
}

/*
Fonction wrapper utilisée par makecontext.
Elle adapte la signature de la fonction et appelle thread_exit à la fin.
*/
static void thread_start(void *(*func)(void *), void *arg)
{
  #ifdef USE_PREEMPTION                                                                                                                 
      sigset_t mask;                                                                                                                    
      sigemptyset(&mask);                                                                                                             
      sigaddset(&mask, SIGALRM);
      sigprocmask(SIG_UNBLOCK, &mask, NULL);                                                                                            
  #endif
    void *ret = func(arg);
    thread_exit(ret);
}

thread_t thread_self(void)
{
    if (current == NULL)
    {
      init_system();
    }
    return (thread_t)current;
}

int thread_create(thread_t *newthread, void *(*func)(void *), void *arg)
{
    sigset_t old;
    block_sigvtalrm(&old);

    if (current == NULL)
    {
        thread_cold_t *tc = THREAD_COLD(t);
        tc->stack_base = NULL;
        tc->waiting = NULL;
        tc->retval = func(arg);
        t->state = FINISHED;
        *newthread = (thread_t)t;
        return 0;
    }

    t->stack_size = STACK_SIZE;
    t->stack = malloc(t->stack_size);
    if (!t->stack)
    {
        free(t);
        restore_sigmask(&old);
        return -1;
    }

    t->state = READY;
    t->retval = NULL;
    t->waiting = NULL;
    t->func = func;
    t->func_arg = arg;

    t->valgrind_stackid = VALGRIND_STACK_REGISTER(t->stack, t->stack + t->stack_size);

    void *old_rsp = NULL;
    void *top = t->stack + t->stack_size;

    asm("mov %%rsp, %0" : "=r"(old_rsp));
    asm("mov %0, %%rsp" : : "r"(top));

    if (_setjmp(t->env) == 0)
    {
        asm("mov %0, %%rsp" : : "r"(old_rsp));
    }
    else
    {
        thread_start(current->func, current->func_arg);
    }

    sched_enqueue(t);

    *newthread = (thread_t)t;
    restore_sigmask(&old);

    return 0;
}

int thread_yield(void)
{
    sigset_t old;
    block_sigvtalrm(&old);

    if (current == NULL)
    {
        init_system();
    }

    if (is_sched_empty())
    {
        restore_sigmask(&old);
        return 0;
    }

    thread_m *prev = current;

    prev->state = READY;
    sched_enqueue(prev);

    thread_hot_t *next = sched_dequeue_fifo();
    next->state = RUNNING;
    current = next;

    if (_setjmp(prev->env) == 0)
    {
      _longjmp(next->env, 1);
    }
    restore_sigmask(&old);
    return 0;
}

int thread_join(thread_t thread, void **retval)
{
    sigset_t old;
    block_sigvtalrm(&old);

    thread_m *t = (thread_m *)thread;

    if (t->state != FINISHED)
    {
        t->waiting = current;
        current->state = BLOCKED;

        thread_hot_t *next = sched_dequeue_lifo();
        if (__builtin_expect(!next, 0))
            return -1;
        }

        next->state = RUNNING;
        thread_m *prev = current;
        current = next;

        if (_setjmp(prev->env) == 0)
        {
          _longjmp(next->env, 1);
        }
    }

    if (retval)
    {
        *retval = t->retval;
    }

    VALGRIND_STACK_DEREGISTER(t->valgrind_stackid);

    free(t->stack);
    free(t);

    restore_sigmask(&old);

    return 0;
}

void thread_exit(void *retval)
{
    sigset_t old;
    block_sigvtalrm(&old);

    current->retval = retval;

    current->state = FINISHED;

    if (current->waiting)
    {
        current->waiting->state = READY;
        sched_enqueue(current->waiting);
    }

    if (is_sched_empty())
    {

    thread_hot_t *next = sched_dequeue_lifo();
    next->state = RUNNING;
    current = next;

    _longjmp(next->env, 1);

    exit(1);
}

int thread_mutex_init(thread_mutex_t *mutex)
{
  thread_mutex_m *m = (thread_mutex_m *)mutex;
  m->locked = 0;
  STAILQ_INIT(&m->waiting);
  return 0;
}

int thread_mutex_destroy(thread_mutex_t *mutex)
{
    (void)mutex;
    return 0;
}

int thread_mutex_lock(thread_mutex_t *mutex)
{
    sigset_t old;
    block_sigvtalrm(&old);

    thread_mutex_m *m = (thread_mutex_m *)mutex;
    if(!m->locked){
      m->locked = 1;
      return 0;
    }

    // bloqué : on se met dans la file et on switch 
    current->state = BLOCKED;
    STAILQ_INSERT_TAIL(&m->waiting, current, link);

    thread_m *next = sched_dequeue();
    thread_m *prev = current;
    next->state = RUNNING;
    current = next;
    if (_setjmp(prev->env) == 0) {
        _longjmp(next->env, 1);
    }

    restore_sigmask(&old);

    return 0;
}

int thread_mutex_unlock(thread_mutex_t *mutex)
{
  sigset_t old;
  block_sigvtalrm(&old);

  thread_mutex_m *m = (thread_mutex_m *)mutex;
  if(STAILQ_EMPTY(&m->waiting)){
   m->locked = 0;
   return 0;
  }
  thread_m *t = STAILQ_FIRST(&m->waiting);
  STAILQ_REMOVE_HEAD(&m->waiting, link);
  t->state = RUNNING;
  sched_enqueue(t);

  restore_sigmask(&old);

  return 0;
}
