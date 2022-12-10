%include "core/cpu_macros.mac"

global syscall_handler

syscall_handler:
    swapgs
 
    mov r8, rsp             ; save process stack to r8 (5th param)
    mov rsp, [gs:0x4]       ; switch to syscall stack

    ; push information (gs, cs, rip, rflags, rip...)
    push qword 0x1b         ; user data segment
    push qword [gs:0x8]     ; saved stack
    push r11                ; saved rflags
    push qword 0x23         ; user code segment 
    push rcx                ; current RIP

    push_all

    mov rcx, r10

    extern syscall_funcs
    call [rax * 8 + syscall_funcs]

    pop_all_syscall

    mov rsp, r8             ; back to user stack
    swapgs
    o64 sysret

