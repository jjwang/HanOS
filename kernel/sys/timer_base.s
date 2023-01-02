.extern apic_send_eoi

.global apic_timer_handler

apic_timer_handler:
    push %rbp
    mov %rsp, %rbp

    push %rax
    push %rdi
    push %rsi
    push %rdx
    push %rcx
    push %r8 
    push %r9 
    push %r10
    push %r11

    call apic_send_eoi

    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rcx
    pop %rdx
    pop %rsi
    pop %rdi
    pop %rax

    pop %rbp

    iretq

