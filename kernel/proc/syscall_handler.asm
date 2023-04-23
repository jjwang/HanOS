%include "sys/cpu_macros.mac"

global syscall_handler
extern k_print_log

syscall_handler:
    push r15                ; store r15 in user stack
    mov r15, rsp            ; save process stack to r15

    ; push information (gs, cs, rip, rflags, rip)
    ; but the below data are not used in current implementation
    push qword 0x3b         ; user data segment
    push r15                ; saved stack
    push r11                ; saved rflags
    push qword 0x43         ; user code segment 
    push rcx                ; current RIP

    ; push all registers
    push_all

    mov rcx, r10

    extern syscall_funcs
    call [rax * 8 + syscall_funcs]

    ; pop all registers except rax which is used for storing return value
    pop_all_syscall

    swapgs

    mov rdx, qword [gs:0x0]  ; return errno in rdx
    mov rsp, r15             ; back to user stack
    pop r15                  ; pop r15 from user stack

    swapgs

    o64 sysret

