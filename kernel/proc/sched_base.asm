%include "sys/cpu_macros.mac"

global enter_context_switch
global exit_context_switch
global force_context_switch
global fork_context_switch

extern do_context_switch

enter_context_switch:
    cli

    push_all

    mov rdi, rsp
    mov rsi, 0

    ; Will call exit_context_switch in the end of implementation
    call do_context_switch

    ; If we reach here, context switch failed (abort path)
    add rsp, 120            ; Undo push_all (15*8=120 bytes)

    ; Trigger Page Fault: Access an unmapped address (0x0 = NULL, always invalid in OS)
    ; MOV from 0x0 will cause PF (CR2 will store 0x0 for debugging)
    mov rax, qword [0x0]    ; Explicitly access invalid memory -> #PF exception

    iretq                   ; Fallback (unreachable in normal flow)

exit_context_switch:
    ; Need to set CR3 here
    test rsi, rsi
    jz .dont_load_cr3
    mov cr3, rsi
.dont_load_cr3:

    mov rsp, rdi            ; Switch to new process's stack pointer
    pop_all                 ; Restore 15 registers (reverse of push_all)
    iretq                   ; Interrupt return (jump to new process's RIP)

force_context_switch:
    cli

    mov rax, rsp

    push qword 0x30         ; RSP + 0x20: SS
    push rax                ; RSP + 0x18: RSP
    push qword 0x202        ; RSP + 0x10: RFLAGS
    push qword 0x28         ; RSP + 0x08: CS
    mov rax, .exit          ; RSP + 0x00: RIP
    push rax

    push_all

    mov rdi, rsp
    mov rsi, 1
  
    call do_context_switch

    add rsp, 120
    mov rax, qword [0x0]    ; Explicitly access invalid memory -> #PF exception

    iretq

.exit:
    ret

fork_context_switch:
    cli 

    mov rax, rsp 

    push qword 0x30
    push rax 
    push qword 0x202
    push qword 0x28
    mov rax, .exit
    push rax 

    push_all

    mov rdi, rsp 
    mov rsi, 2
  
    call do_context_switch

    add rsp, 120 
    mov rax, qword [0x0]    ; Explicitly access invalid memory -> #PF exception

    iretq

.exit:
    ret
