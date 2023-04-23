/* Remember H(a, b, c, d, e, f, g, h);
 * a->%rdi, b->%rsi, c->%rdx, d->%rcx, e->%r8, f->%r9, h->8(%esp)
 */

.extern exc_handler_proc

.macro pusham
    cld
    push %rax
    push %rbx
    push %rcx
    push %rdx
    push %rsi
    push %rdi
    push %rbp
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15
.endm

.macro popam
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rbp
    pop %rdi
    pop %rsi
    pop %rdx
    pop %rcx
    pop %rbx
    pop %rax
.endm

.macro exc_noerrcode excno
.global exc\excno
exc\excno:
    pushq (5 * 8)(%rsp)
    pushq (5 * 8)(%rsp)
    pushq (5 * 8)(%rsp)
    pushq (5 * 8)(%rsp)
    pushq (5 * 8)(%rsp)

    pusham

    mov $\excno, %rdi
    mov %rsp, %rsi
    /* Pass dummy error code */
    movq $0, %rdx

    call exc_handler_proc
    jmp .exc_end
.endm

.macro exc_errcode excno
.global exc\excno
exc\excno:
    pushq (5 * 8)(%rsp)
    pushq (5 * 8)(%rsp)
    pushq (5 * 8)(%rsp)
    pushq (5 * 8)(%rsp)
    pushq (5 * 8)(%rsp)

    pusham

    mov $\excno, %rdi
    mov %rsp, %rsi
    movq (20 * 8)(%rsp), %rdx

    call exc_handler_proc
    jmp .exc_end
.endm

.exc_end:
    popam
    addq $40, %rsp
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
    pushq (6 * 8)(%rsp)
    pushq (6 * 8)(%rsp)
    pushq (6 * 8)(%rsp)
    pushq (6 * 8)(%rsp)
    pushq (6 * 8)(%rsp)

    pusham

    mov $\irqno + 32, %rdi
    mov %rsp, %rsi
    /* Pass dummy error code */
    movq $0, %rdx

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
