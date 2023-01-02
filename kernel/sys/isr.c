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
#include <sys/isr_base.h>
#include <sys/panic.h>
#include <sys/cpu.h>

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

void exc_handler_proc(uint64_t errcode, uint64_t excno)
{
    /* IRQ7 should be skipped */
    if (excno == IRQ7) {
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

    kpanic("Unhandled Exception: %s (%d). Error Code: %d (0x%x).\n",
                 exceptions[excno], excno, errcode, errcode);
    while (true)
        ;
}
