

.text

/* STACK_FREELIST_OFFSET = STACK_SIZE - 8 octets (slot intrusif en bas de
 * pile pour la free-list LIFO). On reflète ici la macro C
 * STACK_FREELIST_PTR(base) de thread_common.h. STACK_SIZE peut être
 * surchargé via -DSTACK_SIZE au build (cf. Makefile, STACK_FLAG passé
 * aussi aux .S). */
#ifndef STACK_SIZE
#define STACK_SIZE (64 * 1024)
#endif
#define STACK_FREELIST_OFFSET (STACK_SIZE - 8)

.hidden thread_entry

// void context_switch(void **old_rsp, void *new_rsp)
// .hidden evite le passage par PLT (meme shared library)

.globl context_switch
.hidden context_switch
.type context_switch, @function
context_switch:
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq  %rsp, (%rdi)      // *old_rsp = rsp
    movq  %rsi, %rsp        // rsp = new_rsp
    popq  %r15
    popq  %r14
    popq  %r13
    popq  %r12
    popq  %rbp
    popq  %rbx
    retq
.size context_switch, .-context_switch

// void context_restore(void *new_rsp) __attribute__((noreturn))

.globl context_restore
.hidden context_restore
.type context_restore, @function
context_restore:
    movq  %rdi, %rsp        // rsp = new_rsp
    popq  %r15
    popq  %r14
    popq  %r13
    popq  %r12
    popq  %rbp
    popq  %rbx
    retq
.size context_restore, .-context_restore


.globl thread_trampoline
.hidden thread_trampoline
.type thread_trampoline, @function
thread_trampoline:
    movq  %r12, %rdi
    movq  %r13, %rsi
    call  thread_entry
    ud2
.size thread_trampoline, .-thread_trampoline

	// thread_yield inlined
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
    movq  8(%rdi), %rsi          // next = current->sched_next
    cmpq  %rsi, %rdi
    je    .Lyield_alone

    // Load next->rsp — also prefetches cache line for stack swap
    movq  (%rsi), %rcx
    testq %rcx, %rcx
    jz    .Lyield_lazy

    movq  %rsi, current(%rip)    // current = next

    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    movq  %rsp, (%rdi)           // prev->rsp = rsp
    movq  %rcx, %rsp             // rsp = next->rsp (already loaded)

    popq  %r15
    popq  %r14
    popq  %r13
    popq  %r12
    popq  %rbp
    popq  %rbx

#ifndef MULTICORE
    /* Consommation de pending_dead_stack post-context_switch.
     * Inliné ici pour éviter un wrapper C (call/ret + endbr64).
     * Hot path : pending_dead_stack est NULL la plupart du temps
     * (uniquement non-NULL au yield qui suit un thread_exit).
     *
     * En MULTICORE, thread_yield_asm n'est pas appelé (thread_mn.c a son
     * propre scheduling) et les symboles pending_dead_stack/stack_free_head
     * ne sont pas exposés — donc on omet ce bloc. */
    movq  pending_dead_stack(%rip), %rdx
    testq %rdx, %rdx
    jnz   .Lyield_release_pending
#endif
    xorl  %eax, %eax
    retq

#ifndef MULTICORE
    .p2align 4
.Lyield_release_pending:
    movq  stack_free_head(%rip), %rcx
    movq  %rcx, STACK_FREELIST_OFFSET(%rdx)   // *(stack + STACK_SIZE - 8) = old head
    movq  %rdx, stack_free_head(%rip)
    movq  $0, pending_dead_stack(%rip)
    xorl  %eax, %eax
    retq
#endif

.Lyield_alone:
.Lyield_inline_noop:
    /* Pas de context_switch : pending_dead_stack ne peut pas avoir été
     * posé depuis le dernier yield/exit du thread courant — la mise en
     * place se fait dans thread_exit du thread précédent, dont le
     * context_restore retourne dans une frame thread_yield_asm
     * (consommée à la sortie ci-dessus). */
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
    // rsi = next thread needing lazy stack allocation
    pushq %rdi
    pushq %rsi
    subq  $8, %rsp               // align stack to 16
    movq  %rsi, %rdi
    call  lazy_stack_alloc
    addq  $8, %rsp
    popq  %rsi
    popq  %rdi
    jmp   .Lyield_fast

.size thread_yield_asm, .-thread_yield_asm

#if !defined(USE_PREEMPTION) && !defined(USE_PRIORITY)
/* Mono sans préemption : alias direct, pas de wrapper C. La consommation
 * de pending_dead_stack est inline ci-dessus. */
.globl thread_yield
.type thread_yield, @function
    .set thread_yield, thread_yield_asm
.size thread_yield, .-thread_yield_asm
#endif


// Marque le stack comme non-exécutable (évite le warning linker)
.section .note.GNU-stack,"",@progbits
