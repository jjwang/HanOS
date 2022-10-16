%include "core/cpu_macros.mac"

global syscall_handler

syscall_handler:
    swapgs
    
    mov [gs:0x8], rsp       ; save process stack
    mov rsp, [gs:0x0]       ; switch to syscall stack

    push_all

    mov rcx, r10

    extern syscall_funcs
    call [rax * 8 + syscall_funcs]

    pop_all_syscall

    mov rsp, [gs:0x8]       ; back to user stack
    swapgs
    sti
    o64 sysret

