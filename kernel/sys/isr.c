/**-----------------------------------------------------------------------------

 @file    isr.c
 @brief   Implementation of ISR related functions
 @details
 @verbatim

  The x86 architecture is an interrupt driven system. Only a common interrupt
  handling function is implemented.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdbool.h>
#include <stdint.h>

#include <base/klog.h>
#include <sys/isr_base.h>
#include <sys/panic.h>
#include <sys/cpu.h>
#include <proc/sched.h>
#include <proc/task.h>

static char* exceptions[] = {
    [0] = "Division by Zero",
    [1] = "Debug",
    [2] = "Non Maskable Interrupt",
    [3] = "Breakpoint",
    [4] = "Overflow",
    [5] = "Bound Range Exceeded",
    [6] = "Invalid opcode",
    [7] = "Device Not Available",
    [8] = "Double Fault",
    [10] = "Invalid TSS",
    [11] = "Segment Not Present",
    [12] = "Stack Exception",
    [13] = "General Protection Fault",
    [14] = "Page Fault",
    [16] = "x87 Floating Point Exception",
    [17] = "Alignment Check",
    [18] = "Machine Check",
    [19] = "SIMD Floating Point Exception",
    [20] = "Virtualization Exception",
    [30] = "Security Exception",
    [32] = "Reserved",
    [33] = "Reserved",
    [34] = "Reserved",
    [35] = "Reserved",
    [36] = "Reserved",
    [37] = "Reserved",
    [38] = "Reserved",
    [39] = "Reserved",
    [40] = "Reserved",
    [41] = "Reserved",
    [42] = "Reserved",
    [43] = "Reserved",
    [44] = "Reserved"
};

static volatile exc_handler_t handlers[256] = { 0 };

void exc_register_handler(uint64_t id, exc_handler_t handler)
{
    handlers[id] = handler;
}

void exc_handler_proc(uint64_t excno, task_regs_t *tr, uint64_t errcode)
{
    /* IRQ7 should be skipped */
    if (excno == IRQ7) {
        return;
    }

    /* IRQ128 is used for system call */
    if (excno == IRQ128) {
        klogi("IRQ: received software interrupt of 0x80 for system call.\n");
        return;
    }

    /* Process other exceptions and interrupts */
    exc_handler_t handler = handlers[excno];

    if (handler != 0) {
        handler();
        /* If the IRQ came from the Master PIC, it is sufficient to issue EOI
         * command only to the Master PIC; however if the IRQ came from the
         * Slave PIC, it is necessary to issue EOI to both PIC chips.
         */
        if (excno >= IRQ0 + 8) {
            port_outb(PIC1, PIC_EOI);
            port_outb(PIC2, PIC_EOI);
        } else {
            port_outb(PIC1, PIC_EOI);
        }
        return;
    }   

    task_t *t = sched_get_current_task();
    task_id_t tid = ((t == NULL) ? 0 : t->tid);

    uint64_t cr2val;
    read_cr("cr2", &cr2val);

    uint64_t cr3val;
    read_cr("cr3", &cr3val);


    klogd("Dump registers for exception: \n"
          "RIP   : 0x%x\nCS    : 0x%x\nRFLAGS: 0x%x\n"
          "RSP   : 0x%x\nSS    : 0x%x\n"
          "RAX 0x%x  RBX 0x%x  RCX 0x%x  RDX 0x%x\n"
          "RSI 0x%x  RDI 0x%x  RBP 0x%x\n"
          "R8  0x%x  R9  0x%x  R10 0x%x  R11 0x%x\n"
          "R12 0x%x  R13 0x%x  R14 0x%x  R15 0x%x\n"
          "CR2 0x%x  CR3 0x%x\n",
          tr->rip, tr->cs, tr->rflags, tr->rsp, tr->ss,
          tr->rax, tr->rbx, tr->rcx, tr->rdx, tr->rsi, tr->rdi, tr->rbp,
          tr->r8, tr->r9, tr->r10, tr->r11, tr->r12, tr->r13, tr->r14,
          tr->r15, cr2val, cr3val);

    kpanic("Unhandled Exception of Task #%d: %s (%d). Error Code: %d (0x%x)\n",
           tid, exceptions[excno], excno, errcode, errcode);

    while (true)
        ;
}
