

.text
.hidden thread_entry

/* Layout thread_hot_t (cf. thread_internal.h) — 64 octets, 1 cache line :
 *    0  : rsp
 *    8  : sched_next
 *   16  : saved_rbx
 *   24  : saved_rbp
 *   32  : saved_r12
 *   40  : saved_r13
 *   48  : saved_r14
 *   56  : saved_r15
 *
 * Plutôt que push/pop sur la pile du thread (cold : top de pile dispersé
 * sur ~640 KB pour 10000 threads), on store/load les 6 callee-saved
 * directement dans hot_slab[t]. Le hot_slab est séquentiel (HW prefetcher
 * efficace) et la même cache line contient déjà rsp + sched_next, donc
 * la lecture de next->rsp tire toute la ligne d'un coup. Mesuré : 76 %
 * du temps de yield était sur le 1er pop après mov %rcx, %rsp (cache miss
 * L3 sur la pile cold). */

// void context_switch(thread_hot_t *prev, thread_hot_t *next)

.globl context_switch
.hidden context_switch
.type context_switch, @function
context_switch:
    movq  %rbx, 16(%rdi)
    movq  %rbp, 24(%rdi)
    movq  %r12, 32(%rdi)
    movq  %r13, 40(%rdi)
    movq  %r14, 48(%rdi)
    movq  %r15, 56(%rdi)
    movq  %rsp,   (%rdi)
    movq    (%rsi), %rsp
    movq  16(%rsi), %rbx
    movq  24(%rsi), %rbp
    movq  32(%rsi), %r12
    movq  40(%rsi), %r13
    movq  48(%rsi), %r14
    movq  56(%rsi), %r15
    retq
.size context_switch, .-context_switch

// void context_restore(thread_hot_t *next) __attribute__((noreturn))

.globl context_restore
.hidden context_restore
.type context_restore, @function
context_restore:
    movq    (%rdi), %rsp
    movq  16(%rdi), %rbx
    movq  24(%rdi), %rbp
    movq  32(%rdi), %r12
    movq  40(%rdi), %r13
    movq  48(%rdi), %r14
    movq  56(%rdi), %r15
    retq
.size context_restore, .-context_restore

/* ---- Variantes "raw" pour le scheduler M:N ----
 * Le scheduler MN switche depuis/vers une pile dédiée sched_rsp qui n'a pas
 * de thread_hot_t associé : on garde l'ancien layout push/pop sur stack.
 * Ces variantes sont incompatibles avec context_switch / context_restore :
 * un thread sauvé par l'une doit être restauré par la même. */

// void context_switch_raw(void **old_rsp, void *new_rsp)
.globl context_switch_raw
.hidden context_switch_raw
.type context_switch_raw, @function
context_switch_raw:
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq  %rsp, (%rdi)
    movq  %rsi, %rsp
    popq  %r15
    popq  %r14
    popq  %r13
    popq  %r12
    popq  %rbp
    popq  %rbx
    retq
.size context_switch_raw, .-context_switch_raw

// void context_restore_raw(void *new_rsp) __attribute__((noreturn))
.globl context_restore_raw
.hidden context_restore_raw
.type context_restore_raw, @function
context_restore_raw:
    movq  %rdi, %rsp
    popq  %r15
    popq  %r14
    popq  %r13
    popq  %r12
    popq  %rbp
    popq  %rbx
    retq
.size context_restore_raw, .-context_restore_raw


.globl thread_trampoline
.hidden thread_trampoline
.type thread_trampoline, @function
thread_trampoline:
    movq  %r12, %rdi
    movq  %r13, %rsi
    call  thread_entry
    ud2
.size thread_trampoline, .-thread_trampoline

// int thread_yield(void)

.globl thread_yield_asm
.type thread_yield_asm, @function
    .p2align 4
thread_yield_asm:
    cmpl  $0, inline_executing(%rip)
    jne   .Lyield_inline_noop

    movq  last_created(%rip), %rax
    testq %rax, %rax
    jnz   .Lyield_flush
.Lyield_fast:
    movq  current(%rip), %rdi
    movq  8(%rdi), %rsi
    cmpq  %rsi, %rdi
    je    .Lyield_alone

    movq  (%rsi), %rcx
    testq %rcx, %rcx
    jz    .Lyield_lazy

    /* Prefetch du top de la nouvelle pile : 88.9 % du temps de yield est
     * sur le `ret` final qui lit l'addr de retour cold (mesure perf
     * annotate sur le nouveau hot path, vs 76 % sur pop %r15 dans l'ancien).
     * La latence dispo entre cet endroit et le ret = 1 store + 6 stores
     * regs + 1 store rsp + 1 load rsp + 6 load regs ≈ 15 cycles, ce qui
     * cache partiellement une L2/L3 miss (~12-40 cycles). */
    prefetcht0 (%rcx)

    movq  %rsi, current(%rip)

    movq  %rbx, 16(%rdi)
    movq  %rbp, 24(%rdi)
    movq  %r12, 32(%rdi)
    movq  %r13, 40(%rdi)
    movq  %r14, 48(%rdi)
    movq  %r15, 56(%rdi)
    movq  %rsp,   (%rdi)

    movq  %rcx, %rsp

    movq  16(%rsi), %rbx
    movq  24(%rsi), %rbp
    movq  32(%rsi), %r12
    movq  40(%rsi), %r13
    movq  48(%rsi), %r14
    movq  56(%rsi), %r15

    xorl  %eax, %eax
    retq

.Lyield_alone:
    xorl  %eax, %eax
    retq

.Lyield_inline_noop:
    xorl  %eax, %eax
    retq

    .p2align 4
.Lyield_flush:
    subq  $8, %rsp
    call  flush_last_created_slow
    addq  $8, %rsp
    jmp   .Lyield_fast

    .p2align 4
.Lyield_lazy:
    pushq %rdi
    pushq %rsi
    subq  $8, %rsp
    movq  %rsi, %rdi
    call  lazy_stack_alloc
    addq  $8, %rsp
    popq  %rsi
    popq  %rdi
    jmp   .Lyield_fast

.size thread_yield_asm, .-thread_yield_asm

#if !defined(USE_PREEMPTION) && !defined(USE_PRIORITY)
.globl thread_yield
.type thread_yield, @function
    .set thread_yield, thread_yield_asm
.size thread_yield, .-thread_yield_asm
#endif

.section .note.GNU-stack,"",@progbits
