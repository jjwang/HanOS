%include "sys/cpu_macros.mac"

global enter_context_switch
global exit_context_switch
global force_context_switch

extern do_context_switch
extern lock_release

enter_context_switch:
    push_all

    mov rdi, rsp
    mov rsi, 0

    ; Will call exit_context_switch in the end of implementation
    call do_context_switch

    add rsp, 120
    iretq

exit_context_switch:
    ; Need to set CR3 here
    test rsi, rsi
    jz .dont_load_cr3
    mov cr3, rsi
.dont_load_cr3:

    mov rsp, rdi

    pop_all

    iretq

force_context_switch:
    cli

    mov rax, rsp

    push qword 0x10
    push rax
    push qword 0x202
    push qword 0x08
    mov rax, .exit
    push rax

    push_all

    mov rdi, rsp
    mov rsi, 1
  
    call do_context_switch

    add rsp, 120
    iretq

.exit:
    ret

