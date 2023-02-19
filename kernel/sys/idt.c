/**-----------------------------------------------------------------------------

 @file    idt.c
 @brief   Implementation of idt related functions
 @details
 @verbatim

  The Interrupt Descriptor Table (idt) telling the CPU where the Interrupt
  Service Routines (ISR) are located (one per interrupt vector). The idt
  entries are called gates. It can contain Interrupt Gates, Task Gates and
  Trap Gates. As the first step, only trap gates (exceptions) are
  implemented.

 @endverbatim
  Ref: https://wiki.osdev.org/Interrupt_Descriptor_Table

 **-----------------------------------------------------------------------------
 */
#include <stdint.h>

#include <lib/klog.h>
#include <lib/memutils.h>
#include <sys/isr_base.h>
#include <sys/idt.h>
#include <sys/cpu.h>
#include <sys/panic.h>

static idt_entry_t idt[IDT_ENTRIES];

/* Vector 0x80 is reserved for system calls */ 
static uint8_t available_vector = 0x81;

static idt_entry_t idt_make_entry(uint64_t offset)
{
    return (idt_entry_t) {
        .selector = 0x28,
        .offset_1 = offset & 0xFFFF,
        .offset_2 = (offset >> 16) & 0xFFFF,
        .offset_3 = (offset >> 32) & 0xFFFFFFFF,
        .ist = 0,
        .type_attributes = IDT_DEFAULT_TYPE_ATTRIBUTES
    };  
}

void idt_set_handler(uint8_t vector, void* handler)
{
    idt[vector] = idt_make_entry((uint64_t)handler);
}

uint8_t idt_get_available_vector(void)
{
    available_vector++;
    if (available_vector == 0)
        kpanic("IRQ vector is not available.\n");

    return available_vector;
}

void irq_set_mask(uint8_t line)
{
    uint16_t port;
    uint8_t value;
 
    if(line < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        line -= 8;
    }
    value = port_inb(port) | (1 << line);
    port_outb(port, value);
    klogv("IRQ: Send %s with 0x%02x\n", (port == PIC1_DATA ? "PIC1_DATA" : "PIC2_DATA"), value);
}
 
void irq_clear_mask(uint8_t line)
{
    uint16_t port;
    uint8_t value;
 
    if(line < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        line -= 8;
    }
    value = port_inb(port) & ~(1 << line);
    port_outb(port, value);
    klogv("IRQ: Send %s with 0x%02x\n", (port == PIC1_DATA ? "PIC1_DATA" : "PIC2_DATA"), value);
}

void idt_init()
{
    /* start 8259A PIC initialization */
    port_outb(PIC1, 0x11);
    port_outb(PIC2, 0x11);
    
    /* set IRQ base numbers for each PIC */
    port_outb(PIC1_DATA, IRQ0);
    port_outb(PIC2_DATA, IRQ0 + 8);

    /* use IRQ number 2 to relay IRQs from the slave PIC */
    port_outb(PIC1_DATA, 0x04);
    port_outb(PIC2_DATA, 0x02);
    
    /* finish initialization */
    port_outb(PIC1_DATA, 0x01);
    port_outb(PIC2_DATA, 0x01);

    /* Mask all interrupts */
    port_outb(PIC1_DATA, 0xFF);
    port_outb(PIC2_DATA, 0xFF);

    for(size_t i = 0; i < IDT_ENTRIES; i++) {
        memset(&idt[i], 0, sizeof(idt_entry_t));
    }

    /* Exceptions */
    idt[0] = idt_make_entry((uint64_t)&exc0);
    idt[1] = idt_make_entry((uint64_t)&exc1);
    idt[2] = idt_make_entry((uint64_t)&exc2);
    idt[3] = idt_make_entry((uint64_t)&exc3);
    idt[4] = idt_make_entry((uint64_t)&exc4);
    idt[5] = idt_make_entry((uint64_t)&exc5);
    idt[6] = idt_make_entry((uint64_t)&exc6);
    idt[7] = idt_make_entry((uint64_t)&exc7);
    idt[8] = idt_make_entry((uint64_t)&exc8);
    idt[10] = idt_make_entry((uint64_t)&exc10);
    idt[11] = idt_make_entry((uint64_t)&exc11);
    idt[12] = idt_make_entry((uint64_t)&exc12);
    idt[13] = idt_make_entry((uint64_t)&exc13);
    idt[14] = idt_make_entry((uint64_t)&exc14);
    idt[16] = idt_make_entry((uint64_t)&exc16);
    idt[17] = idt_make_entry((uint64_t)&exc17);
    idt[18] = idt_make_entry((uint64_t)&exc18);
    idt[19] = idt_make_entry((uint64_t)&exc19);
    idt[20] = idt_make_entry((uint64_t)&exc20);
    idt[30] = idt_make_entry((uint64_t)&exc30);

    /* Hardware interrupts */
    idt[32] = idt_make_entry((uint64_t)&irq0);
    idt[33] = idt_make_entry((uint64_t)&irq1);
    idt[34] = idt_make_entry((uint64_t)&irq2);
    idt[35] = idt_make_entry((uint64_t)&irq3);
    idt[36] = idt_make_entry((uint64_t)&irq4);
    idt[37] = idt_make_entry((uint64_t)&irq5);
    idt[38] = idt_make_entry((uint64_t)&irq6);
    idt[39] = idt_make_entry((uint64_t)&irq7);
    idt[40] = idt_make_entry((uint64_t)&irq8);
    idt[41] = idt_make_entry((uint64_t)&irq9);
    idt[42] = idt_make_entry((uint64_t)&irq10);
    idt[43] = idt_make_entry((uint64_t)&irq11);
    idt[44] = idt_make_entry((uint64_t)&irq12);

    idt_register_t idt_register = {
            .size = sizeof(idt_entry_t) * IDT_ENTRIES - 1,
            .offset = (uint64_t)idt };
    asm volatile ("lidt %0" : : "m"(idt_register));

    /* Set soft interrupt handler of 0x80 for system call */
    idt_set_handler(0x80, irq128);

    isr_enable_interrupts();

    klogi("IDT initialization finished\n");
}

