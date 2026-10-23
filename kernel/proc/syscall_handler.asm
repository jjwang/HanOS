%include "arch/x64/cpu_macros.mac"

global syscall_handler
extern k_print_log
extern syscall_post

syscall_handler:
    ; The user context leaves GS.base at zero. Swap in the per-CPU pointer
    ; before touching the per-CPU slots.
    swapgs

    ; Run the handler on the process kernel stack: save the caller RSP in the
    ; per-CPU slot, then switch RSP to the kernel stack top.
    mov [gs:16], rsp        ; save user rsp
    mov rsp, [gs:24]        ; kernel stack top

    push r15                ; store r15 in kernel stack
    mov r15, rsp            ; save process stack to r15

    ; push information (ss, rsp, rflags, cs, rip)
    push qword 0x3b         ; user data segment
    push qword [gs:16]      ; saved user rsp
    push r11                ; saved rflags
    push qword 0x43         ; user code segment 
    push rcx                ; current RIP

    ; push all registers
    push_all

    mov rcx, r10

    extern syscall_funcs
    cmp rax, 0x480          ; SYSCALL_TABLE_SIZE (keep in sync with syscall.h)
    jae .enosys
    mov rbx, [rax * 8 + syscall_funcs]
    test rbx, rbx
    jz .enosys
    mov [gs:8], rsp         ; expose the saved register frame to the handler
    call rbx
    jmp .post
.enosys:
    mov rax, -38            ; -ENOSYS
.post:
    ; Let a queued signal rewrite the return frame before it is popped.
    mov rdi, rax
    mov rsi, rsp
    call syscall_post
.dispatched:
    ; pop all registers except rax which is used for storing return value
    pop_all_syscall

    ; Linux convention: a handler reports an error as -1 plus a per-CPU errno.
    ; Convert that to a negative errno in rax. Keep -1 when errno is 0 so a
    ; handler that uses -1 as a non-error result is not turned into success.
    mov rdx, qword [gs:0x0]  ; errno
    cmp rax, -1
    jne .done
    test rdx, rdx
    jz .done
    neg rdx
    mov rax, rdx
.done:
    ; SYSRET leaves GS.base at zero on this path. Copy the current kernel
    ; GS.base into KERN_GS_BASE, then swap it back for the next syscall.
    push r10
    push rcx
    push rdx
    mov r10, rax
    mov ecx, 0xc0000101
    rdmsr
    mov ecx, 0xc0000102
    wrmsr
    mov rax, r10
    pop rdx
    pop rcx
    pop r10

    ; r15 points at the frame's r15 slot; the user rsp lives 16 below it.
    mov rsp, [r15 - 16]
    mov r15, [r15]

    swapgs

    o64 sysret

