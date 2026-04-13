

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

// Marque le stack comme non-exécutable (évite le warning linker)
.section .note.GNU-stack,"",@progbits
