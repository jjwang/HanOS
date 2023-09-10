global _start

extern main
extern sys_exit

_start:
    pop rdi
    mov rsi, rsp

    call main

    mov rax, 23     ; exit
    mov rdi, 0

    syscall

    ret

