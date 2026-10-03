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

#include <lib/klog.h>
#include <arch/x64/isr_base.h>
#include <arch/x64/panic.h>
#include <arch/x64/cpu.h>
#include <arch/x64/apic.h>
#include <arch/x64/ioapic.h>
#include <arch/x64/serial.h>
#include <arch/x64/timer.h>
#include <proc/sched.h>
#include <proc/process.h>
#include <proc/syscall.h>
#include <ipc/irq.h>
#include <mm/uaccess.h>

#include <printf.h>

static char *exceptions[] = {
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

/* Signal for a CPU exception that reached user mode, or 0 when the exception
 * has no process context. */
static int32_t fault_signal(uint64_t excno)
{
    switch (excno) {
    case 0:
        return SIGFPE;
    case 6:
        return SIGILL;
    case 7:
    case 16:
    case 19:
        return SIGFPE;
    case 17:
        return SIGBUS;
    case 13:
    case 14:
        return SIGSEGV;
    default:
        return 0;
    }
}

/* A user-mode exception kills the process instead of the core. */
static _Noreturn void kill_faulting_process(uint64_t excno, exception_regs_t * tr)
{
    int32_t sig = fault_signal(excno);
    const char *name = "Unknown";

    if (sig == 0)
        sig = SIGSEGV;

    if (excno < sizeof(exceptions) / sizeof(exceptions[0]) && exceptions[excno])
        name = exceptions[excno];

    kloge("User exception %s (%ld) at RIP 0x%016lx, pid %ld: killing process\n",
          name, excno, tr->rip, sched_get_pid());

    k_exit_group(128 + sig);

    for (;;)
        asm volatile ("hlt");
}

/* End an external interrupt at the controller that delivered it. An I/O APIC
 * entry needs a local-APIC EOI; the 8259 path needs a PIC EOI. */
static void irq_eoi(uint64_t excno)
{
    if (ioapic_available()) {
        apic_send_eoi();
        return;
    }

    if (excno >= IRQ0 + 8 && excno < IRQ128) {
        port_outb(PIC1, PIC_EOI);
        port_outb(PIC2, PIC_EOI);
    } else {
        port_outb(PIC1, PIC_EOI);
    }
}

void exc_register_handler(uint64_t id, exc_handler_t handler)
{
    handlers[id] = handler;
}

void exc_handler_proc(
    uint64_t excno, exception_regs_t * tr, uint64_t errcode,
    uint64_t original_rsp, uint8_t is_userspace)
{
    (void)original_rsp;
    (void)is_userspace;

    /* IRQ7 should be skipped */
    if (excno == IRQ7) {
        return;
    }

    /* Page Fault */
    if (excno == 14) {
        /* A fault on a user-access instruction is recoverable: resume at its
         * fixup so the copy reports -EFAULT instead of killing the kernel. */
        uint64_t fixup = uaccess_find_fixup(tr->rip);
        if (fixup != 0) {
            tr->rip = fixup;
            return;
        }

        uint64_t cr2val;
        read_cr("cr2", &cr2val);

        if ((tr->cs & 3) == 3) {
            kloge("User page fault at RIP 0x%016lx, addr 0x%016lx\n",
                  tr->rip, cr2val);
            kill_faulting_process(excno, tr);
        }

        asm volatile("cli");    /* Disable to prevent nested interrupts */
        apic_timer_stop();      /* Mask APIC timer IRQ on current CPU */
        klogi("APIC: Timer IRQ masked to stop interrupt storm.\n");

        uint64_t cr3val;
        read_cr("cr3", &cr3val);

        kloge("Dump registers for exception: \n"
              "RIP   : 0x%016lx\nCS    : 0x%016lx\nRFLAGS: 0x%016lx\n"
              "RSP   : 0x%016lx\nSS    : 0x%016lx\n"
              "RAX 0x%016lx  RBX 0x%016lx  RCX 0x%016lx  RDX 0x%016lx\n"
              "RSI 0x%016lx  RDI 0x%016lx  RBP 0x%016lx\n"
              "R8  0x%016lx  R9  0x%016lx  R10 0x%016lx  R11 0x%016lx\n"
              "R12 0x%016lx  R13 0x%016lx  R14 0x%016lx  R15 0x%016lx\n"
              "CR2 0x%016lx  CR3 0x%016lx\n",
              tr->rip, tr->cs, tr->rflags, tr->rsp, tr->ss,
              tr->rax, tr->rbx, tr->rcx, tr->rdx, tr->rsi, tr->rdi, tr->rbp,
              tr->r8, tr->r9, tr->r10, tr->r11, tr->r12, tr->r13, tr->r14,
              tr->r15, cr2val, cr3val);

        kloge("Unhandled Exception: Page Fault (14).\n");
        dump_backtrace();
        for(;;) {
            asm volatile ("hlt");
        }
    }

    /* IRQ128 is used for system call */
    if (excno == IRQ128) {
        klogi
            ("IRQ: received software interrupt of 0x80 for system call.\n");
        return;
    }

    /* Some of IRQ130+ are used for scheduler */
    if (excno > IRQ128) {
        klogi("IRQ: received software interrupt of 0x%02lx\n", excno);
    }

    /* If a userspace driver has bound this line, deliver a notification and
     * let the driver service the device instead of running the in-kernel
     * handler. */
    if (excno >= IRQ0 && excno < IRQ0 + 16 && irq_deliver(excno - IRQ0)) {
        irq_eoi(excno);
        return;
    }

    /* Process other exceptions and interrupts */
    exc_handler_t handler = handlers[excno];

    if (handler != 0) {
        handler();
        irq_eoi(excno);
        return;
    }

    /* A user-mode exception with no handler kills the process, not the core. */
    if ((tr->cs & 3) == 3) {
        kill_faulting_process(excno, tr);
    }

    uint64_t cr2val;
    read_cr("cr2", &cr2val);

    uint64_t cr3val;
    read_cr("cr3", &cr3val);

    klogd("Dump registers for exception: \n"
          "RIP   : 0x%016lx\nCS    : 0x%016lx\nRFLAGS: 0x%016lx\n"
          "RSP   : 0x%016lx\nSS    : 0x%016lx\n"
          "RAX 0x%016lx  RBX 0x%016lx  RCX 0x%016lx  RDX 0x%016lx\n"
          "RSI 0x%016lx  RDI 0x%016lx  RBP 0x%016lx\n"
          "R8  0x%016lx  R9  0x%016lx  R10 0x%016lx  R11 0x%016lx\n"
          "R12 0x%016lx  R13 0x%016lx  R14 0x%016lx  R15 0x%016lx\n"
          "CR2 0x%016lx  CR3 0x%016lx\n",
          tr->rip, tr->cs, tr->rflags, tr->rsp, tr->ss,
          tr->rax, tr->rbx, tr->rcx, tr->rdx, tr->rsi, tr->rdi, tr->rbp,
          tr->r8, tr->r9, tr->r10, tr->r11, tr->r12, tr->r13, tr->r14,
          tr->r15, cr2val, cr3val);

    kpanic("Unhandled Exception: %s (%ld). Error Code: %ld (0x%016lx)\n",
         exceptions[excno], excno, errcode, errcode);

    for(;;) {
        asm volatile ("hlt");
    }
}
