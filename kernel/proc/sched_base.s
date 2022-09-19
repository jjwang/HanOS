.global enter_context_switch
.global exit_context_switch

.extern do_context_switch

enter_context_switch:
    push %rax
    push %rbx
    push %rcx
    push %rdx
    push %rsi
    push %rdi
    push %rbp
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15

    mov %rsp, %rdi

    /* Will call exit_context_switch in the end of implementation */
    call do_context_switch

    add $120, %rsp
    iretq

exit_context_switch:
    /* Need to set CR3 here */
    test %rsi, %rsi
    jz .dont_load_cr3
    mov %cr3, %rsi
.dont_load_cr3:

    mov %rdi, %rsp

    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rbp
    pop %rdi
    pop %rsi
    pop %rdx
    pop %rcx
    pop %rbx
    pop %rax

    iretq
