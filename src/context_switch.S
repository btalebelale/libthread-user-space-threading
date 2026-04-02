// Context switch minimal pour x86-64 (System V ABI)
// Sauvegarde uniquement les registres callee-saved : rbx, rbp, r12-r15, rsp
// Pas de sauvegarde signal mask (pas de syscall) → ~10-20x plus rapide que swapcontext

.text
.hidden thread_entry

// void context_switch(void **old_rsp, void *new_rsp)
// Sauvegarde le contexte courant sur la pile, stocke rsp dans *old_rsp,
// charge new_rsp et restaure le contexte de destination.
.globl context_switch
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
// Charge new_rsp et restaure le contexte sans sauvegarder l'ancien.
// Utilisé par thread_exit (pas besoin de sauvegarder le thread sortant).
.globl context_restore
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

// Point d'entrée pour les nouveaux threads.
// Après context_switch → ret, on arrive ici avec :
//   r12 = func, r13 = arg (placés lors de l'initialisation du stack)
// Appelle thread_entry(func, arg) qui ne retourne jamais.
.globl thread_trampoline
.type thread_trampoline, @function
thread_trampoline:
    movq  %r12, %rdi         // arg1 = func
    movq  %r13, %rsi         // arg2 = arg
    call  thread_entry
    ud2                       // unreachable
.size thread_trampoline, .-thread_trampoline

// Marque le stack comme non-exécutable (évite le warning linker)
.section .note.GNU-stack,"",@progbits
