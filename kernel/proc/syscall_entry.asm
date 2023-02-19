global syscall_entry

;
; Register %rax contains the result of syscall
;
syscall_entry:
    ; When entering the syscall_handle, the cpu puts the previous value of
    ; RIP in RCX and the previous value of RFLAGS in R11. The processor also
    ; uses them to reset the value of RIP and RFLAGS when the syscall returns.
    push r11
    push rcx

    ;
    ; From System V Application Binary Interface AMD64 Architecture Processor 57
    ; Supplement Draft Version 0.99.6, Section AMD64 Linux Kernel Conventions
    ;
    ; 1. User-level applications use as integer registers for passing the
    ;    sequence %rdi, %rsi, %rdx, %rcx, %r8 and %r9. The kernel interface
    ;    uses %rdi, %rsi, %rdx, %r10, %r8 and %r9.
    ; 2. A system-call is done via the syscall instruction. The kernel
    ;    destroys registers %rcx and %r11.
    ; 3. The number of the syscall has to be passed in register %rax.
    ; 4. System-calls are limited to six arguments, no argument is passed
    ;    directly on the stack.
    ; 5. Returning from the syscall, register %rax contains the result of the
    ;    system-call. A value in the range between -4095 and -1 indicates an
    ;    error, it is -errno.
    ; 6. Only values of class INTEGER or class MEMORY are passed to the kernel.
    ;
    mov rax, rdi            ; syscall number
    mov rdi, rsi            ; 1st arg
    mov rsi, rdx            ; 2nd arg
    mov rdx, rcx            ; 3rd arg
    mov r10, r8             ; 4th arg
    mov r8, r9              ; 5th arg
    mov r9, [rsp + 0x18]    ; 6th arg

    syscall

    pop rcx
    pop r11

    ret

