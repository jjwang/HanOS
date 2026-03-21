/* -----------------------------------------------------------------------------
 * x86-64 SMP-Compatible Exception/IRQ Handlers (Userspace + Kernelspace)
 * Highlights:
 *  1. Valid x86-64 addressing mode (replaced invalid (%rcx, %rsp) syntax)
 *  2. Support userspace/kernelspace privilege level (CPL) detection
 *  3. Dynamic stack offset calculation for different privilege levels
 *  4. Correct iretq return for userspace (RSP+SS handling)
 * Notes: 
 *  - x86-64 calling convention: a->%rdi, b->%rsi, c->%rdx, d->%rcx, 
 *    e->%r8, f->%r9, h->8(%rsp)
 *  - Userspace CPL=3 (CS=0x23), Kernelspace CPL=0 (CS=0x08)
 * -------------------------------------------------------------------------- */
.extern exc_handler_proc

/* -----------------------------------------------------------------------------
 * Helper macro: Detect current privilege level (CPL)
 * Output: 
 *  - %r12 = original RSP at exception entry
 *  - %r13 = 1 (userspace) / 0 (kernelspace)
 * -------------------------------------------------------------------------- */
.macro detect_cpl
    /* Save original RSP (exception entry point) */
    mov %rsp, %r12
    
    /* Extract CPL from CS register (low 2 bits of CS) */
    mov 8(%r12), %rax       /* CS is at 8 bytes offset in exception stack */
    and $0x3, %rax          /* Isolate CPL bits */
    mov $0, %r13            /* Default: kernelspace (CPL=0) */
    cmp $3, %rax            /* Check if userspace (CPL=3) */
    je 1f
    jmp 2f
1:
    mov $1, %r13            /* Mark as userspace */
2:
.endm

/* -----------------------------------------------------------------------------
 * pusham: Save all 64-bit registers (general + segment) for SMP
 * Compatible with both kernel/userspace
 * -------------------------------------------------------------------------- */
.macro pusham
    /* Save segment registers (critical for SMP: GS = current CPU) */
    push %fs
    push %gs

    /* Save 64-bit general purpose registers (symmetric with popam) */
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

/* -----------------------------------------------------------------------------
 * popam: Restore all 64-bit registers (reverse of pusham)
 * -------------------------------------------------------------------------- */
.macro popam
    /* Restore general purpose registers (reverse order) */
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

    /* Restore segment registers (reverse order) */
    pop %gs
    pop %fs
.endm

/* -----------------------------------------------------------------------------
 * exc_noerrcode: Exception handler (no hardware error code)
 * Fixed: Valid addressing mode for dynamic offset calculation
 * -------------------------------------------------------------------------- */
.macro exc_noerrcode excno
.global exc\excno
exc\excno:
    pushq $0                /* Dummy error code (align stack) */
    cld                     /* Clear direction flag (string ops) */
    pusham                  /* Save full context */

    /* Detect privilege level (kernel/userspace) */
    detect_cpl

    /* Calculate dynamic error code offset (VALID x86-64 syntax) */
    mov %rsp, %rax          /* Base = current stack pointer */
    mov $136, %rcx          /* Kernelspace offset (136 bytes) */
    cmp $1, %r13            /* Check if userspace */
    je 1f
    jmp 2f
1:
    mov $152, %rcx          /* Userspace offset (152 bytes) */
2:
    add %rcx, %rax          /* Calculate final address: rsp + offset */

    /* Pass args to C handler (x86-64 calling convention) */
    mov $\excno, %rdi       /* 1st arg: Exception number */
    mov %rsp, %rsi          /* 2nd arg: Current stack pointer */
    movq (%rax), %rdx       /* 3rd arg: Dummy error code (VALID syntax) */
    mov %r12, %r8           /* 4th arg: Original exception RSP */
    mov %r13, %r9           /* 5th arg: Privilege level (0=kernel,1=user) */

    call exc_handler_proc   /* Call C exception handler */
    jmp .exc_end            /* Common cleanup */
.endm

/* -----------------------------------------------------------------------------
 * exc_errcode: Exception handler (with hardware error code)
 * Fixed: Valid addressing mode for dynamic offset calculation
 * -------------------------------------------------------------------------- */
.macro exc_errcode excno
.global exc\excno
exc\excno:
    /* Only apply to Exception 14 (Page Fault) - prevent recursive fault */
    .if \excno == 14
        /* Step 1: Emergency restore kernel segment registers */
        mov $0x10, %rax     /* Kernel data segment selector (refer to task.h) */
        mov %rax, %ds       /* Restore DS to valid kernel data segment */
        mov %rax, %es       /* Restore ES */
        mov %rax, %ss       /* Restore SS (critical for stack operations) */
        xor %rax, %rax          
        mov %rax, %fs       /* Clear FS (user-specific) */
        mov %rax, %gs       /* Clear GS (SMP CPU ID) */
        cli                 /* Disable interrupts to avoid nested faults */
            
        /* Step 2: Pre-save CR2 (page fault address) for C handler */
        push %rax           /* Temp save RAX */
        mov %cr2, %rax      /* Read fault address from CR2 register */
        push %rax           /* Save CR2 to stack (offset: 8(%rsp) after pusham) */
        pop %rax            /* Restore RAX */
    .endif

    cld                     /* Clear direction flag (string ops) */
    pusham                  /* Save full context */

    /* Detect privilege level (kernel/userspace) */
    detect_cpl

    /* Calculate dynamic error code offset (VALID x86-64 syntax) */
    mov %rsp, %rax          /* Base = current stack pointer */
    mov $136, %rcx          /* Kernelspace offset (136 bytes) */
    cmp $1, %r13            /* Check if userspace */
    je 1f
    jmp 2f
1:
    mov $152, %rcx          /* Userspace offset (152 bytes) */
2:
    add %rcx, %rax          /* Calculate final address: rsp + offset */

    /* Pass args to C handler (x86-64 calling convention) */
    mov $\excno, %rdi       /* 1st arg: Exception number */
    mov %rsp, %rsi          /* 2nd arg: Current stack pointer */
    movq (%rax), %rdx       /* 3rd arg: Hardware error code (VALID syntax) */

    /* For Page Fault (exc14): pass CR2 as 4th arg */
    .if \excno == 14
        mov %cr2, %r8       /* 4th arg: Page fault address (CR2) */
        mov %r12, %r9       /* 5th arg: Original exception RSP (shifted) */
        mov %r13, %rcx      /* 6th arg: Privilege level (0=kernel,1=user) */
    .else
        mov %r12, %r8       /* 4th arg: Original exception RSP (normal) */
        mov %r13, %r9       /* 5th arg: Privilege level (0=kernel,1=user) */
    .endif

    call exc_handler_proc   /* Call C exception handler */
    jmp .exc_end            /* Common cleanup */
.endm

/* -----------------------------------------------------------------------------
 * irq_noerrcode: IRQ handler (no error code, SMP + userspace compatible)
 * Fixed: Valid addressing mode for stack operations
 * -------------------------------------------------------------------------- */
.macro irq_noerrcode irqno
.global irq\irqno
irq\irqno:
    pushq $0                /* Dummy error code (align stack) */
    cld                     /* Clear direction flag (string ops) */
    pusham                  /* Save full context */

    /* Detect privilege level (kernel/userspace) */
    detect_cpl

    /* Pass args to C handler (x86-64 calling convention) */
    mov $\irqno + 32, %rdi  /* 1st arg: IRQ number (offset for IDT) */
    mov %rsp, %rsi          /* 2nd arg: Current stack pointer */
    movq $0, %rdx           /* 3rd arg: Dummy error code */
    mov %r12, %r8           /* 4th arg: Original exception RSP */
    mov %r13, %r9           /* 5th arg: Privilege level (0=kernel,1=user) */

    call exc_handler_proc   /* Call C IRQ handler */
    jmp .exc_end            /* Common cleanup */
.endm

/* -----------------------------------------------------------------------------
 * .exc_end: Common cleanup/return (supports userspace + kernelspace)
 * Dynamic stack cleanup based on privilege level
 * -------------------------------------------------------------------------- */
.exc_end:
    popam                   /* Restore full context */
    
    /* Check privilege level for stack cleanup */
    cmp $1, %r13            /* Check if userspace (r13=1) */
    je 1f
    
    /* Kernelspace cleanup: only dummy error code (8 bytes) */
    addq $8, %rsp           
    iretq                   /* Kernelspace return (RIP+CS+RFLAGS) */

1:
    /* Userspace cleanup: dummy error code + RSP+SS (8+16=24 bytes) */
    addq $24, %rsp          
    iretq                   /* Userspace return (RIP+CS+RFLAGS+RSP+SS) */

/* -----------------------------------------------------------------------------
 * Exception Definitions (strict error code classification)
 * -------------------------------------------------------------------------- */
exc_noerrcode   0           /* Divide By Zero */
exc_noerrcode   1           /* Debug */
exc_noerrcode   2           /* NMI */
exc_noerrcode   3           /* Breakpoint */
exc_noerrcode   4           /* Overflow */
exc_noerrcode   5           /* Bound Range Exceeded */
exc_noerrcode   6           /* Invalid Opcode */
exc_noerrcode   7           /* Device Not Available */
exc_errcode     8           /* Double Fault */
exc_errcode     10          /* Invalid TSS */
exc_errcode     11          /* Segment Not Present */
exc_errcode     12          /* Stack-Segment Fault */
exc_errcode     13          /* General Protection Fault */
exc_errcode     14          /* Page Fault */
exc_noerrcode   16          /* x87 Floating-Point Exception */
exc_errcode     17          /* Alignment Check */
exc_noerrcode   18          /* Machine Check */
exc_noerrcode   19          /* SIMD Floating-Point Exception */
exc_noerrcode   20          /* Virtualization Exception */
exc_errcode     30          /* Security Exception */

/* -----------------------------------------------------------------------------
 * IRQ Definitions (hardware IRQs + syscall, SMP/userspace compatible)
 * -------------------------------------------------------------------------- */
irq_noerrcode   0           /* IRQ0: Timer (PIT/APIC) */
irq_noerrcode   1           /* IRQ1: Keyboard */
irq_noerrcode   2           /* IRQ2: Cascade */
irq_noerrcode   3           /* IRQ3: Serial Port 2 */
irq_noerrcode   4           /* IRQ4: Serial Port 1 */
irq_noerrcode   5           /* IRQ5: Parallel Port 2 */
irq_noerrcode   6           /* IRQ6: Floppy Disk */
irq_noerrcode   7           /* IRQ7: Parallel Port 1 */
irq_noerrcode   8           /* IRQ8: RTC Clock */
irq_noerrcode   9           /* IRQ9: ACPI */
irq_noerrcode   10          /* IRQ10: Reserved */
irq_noerrcode   11          /* IRQ11: Reserved */
irq_noerrcode   12          /* IRQ12: PS/2 Mouse */
irq_noerrcode   128         /* IRQ128: Syscall (userspace system calls) */

