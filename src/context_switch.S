

.text
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

#ifndef USE_PREEMPTION                                                                                                                
// thread_yield == thread_yield_asm.
.globl thread_yield                                                                                                                   
.type thread_yield, @function
    .set thread_yield, thread_yield_asm                                                                                               
.size thread_yield, .-thread_yield_asm
#endif 

// Marque le stack comme non-exécutable (évite le warning linker)
.section .note.GNU-stack,"",@progbits
