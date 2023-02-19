/* Remember H(a, b, c, d, e, f, g, h);
 * a->%rdi, b->%rsi, c->%rdx, d->%rcx, e->%r8, f->%r9, h->8(%esp)
 */

.extern exc_handler_proc

.macro exc_noerrcode excno
.global exc\excno
exc\excno:

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

    /* pass dummy error code */
    mov $0, %rdi

    /* pass exception number */
    mov $\excno, %rsi

    call exc_handler_proc
    jmp .exc_end
.endm

.macro exc_errcode excno
.global exc\excno
exc\excno:
    /* pop the error code */
    popq %r12

    push %rbp
    mov %rsp, %rbp

    push %rax
    push %rdi

    /* pass the error code */
    movq %r12, %rdi

    push %rsi
    push %rdx
    push %rcx
    push %r8
    push %r9
    push %r10
    push %r11

    /* pass exception number */
    mov $\excno, %rsi

    call exc_handler_proc
    jmp .exc_end
.endm

.exc_end:
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

exc_noerrcode   0
exc_noerrcode   1
exc_noerrcode   2
exc_noerrcode   3
exc_noerrcode   4
exc_noerrcode   5
exc_noerrcode   6
exc_noerrcode   7
exc_errcode     8
exc_errcode     10
exc_errcode     11
exc_errcode     12
exc_errcode     13
exc_errcode     14
exc_noerrcode   16
exc_errcode     17
exc_noerrcode   18
exc_noerrcode   19
exc_noerrcode   20
exc_errcode     30

.macro irq_noerrcode irqno
.global irq\irqno
irq\irqno:
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

    /* pass dummy error code */
    mov $0, %rdi

    /* pass exception number */
    mov $\irqno + 32, %rsi

    call exc_handler_proc
    jmp .exc_end
.endm

irq_noerrcode   0
irq_noerrcode   1
irq_noerrcode   2
irq_noerrcode   3
irq_noerrcode   4
irq_noerrcode   5
irq_noerrcode   6
irq_noerrcode   7
irq_noerrcode   8
irq_noerrcode   9
irq_noerrcode   10
irq_noerrcode   11
irq_noerrcode   12
irq_noerrcode   128
