/**-----------------------------------------------------------------------------

 @file    isr_base.h
 @brief   Definition of ISR related data structures
 @details
 @verbatim

  The x86 architecture is an interrupt driven system.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <lib/klog.h>

#define PIC1        0x20 /* Master PIC */
#define PIC2        0xA0 /* Slave PIC */
#define PIC1_DATA   (PIC1 + 1)
#define PIC2_DATA   (PIC2 + 1)
#define PIC_EOI     0x20 /* end of interrupt */
#define IRQ_BASE    0x20

/* Hardware interrupts */
#define IRQ0        32
#define IRQ1        33
#define IRQ2        34
#define IRQ3        35
#define IRQ4        36
#define IRQ5        37
#define IRQ6        38
#define IRQ7        39
#define IRQ8        40
#define IRQ9        41
#define IRQ10       42
#define IRQ11       43
#define IRQ12       44

/* Software interrupts */
#define IRQ128      (128 + 32)

typedef void (*exc_handler_t)();
void exc_register_handler(uint64_t id, exc_handler_t handler);

#define isr_enable_interrupts()                     \
    {                                               \
        asm volatile("sti");                        \
    }

#define isr_disable_interrupts()                    \
    {                                               \
        asm volatile("cli");                        \
    }

[[gnu::interrupt]] void exc0(void* p);
[[gnu::interrupt]] void exc1(void* p);
[[gnu::interrupt]] void exc2(void* p);
[[gnu::interrupt]] void exc3(void* p);
[[gnu::interrupt]] void exc4(void* p);
[[gnu::interrupt]] void exc5(void* p);
[[gnu::interrupt]] void exc6(void* p);
[[gnu::interrupt]] void exc7(void* p);
[[gnu::interrupt]] void exc8(void* p);
[[gnu::interrupt]] void exc10(void* p);
[[gnu::interrupt]] void exc11(void* p);
[[gnu::interrupt]] void exc12(void* p);
[[gnu::interrupt]] void exc13(void* p);
[[gnu::interrupt]] void exc14(void* p);
[[gnu::interrupt]] void exc16(void* p);
[[gnu::interrupt]] void exc17(void* p);
[[gnu::interrupt]] void exc18(void* p);
[[gnu::interrupt]] void exc19(void* p);
[[gnu::interrupt]] void exc20(void* p);
[[gnu::interrupt]] void exc30(void* p);

[[gnu::interrupt]] void irq0(void* p);
[[gnu::interrupt]] void irq1(void* p);
[[gnu::interrupt]] void irq2(void* p);
[[gnu::interrupt]] void irq3(void* p);
[[gnu::interrupt]] void irq4(void* p);
[[gnu::interrupt]] void irq5(void* p);
[[gnu::interrupt]] void irq6(void* p);
[[gnu::interrupt]] void irq7(void* p);
[[gnu::interrupt]] void irq8(void* p);
[[gnu::interrupt]] void irq9(void* p);
[[gnu::interrupt]] void irq10(void* p);
[[gnu::interrupt]] void irq11(void* p);
[[gnu::interrupt]] void irq12(void* p);
[[gnu::interrupt]] void irq128(void* p);

