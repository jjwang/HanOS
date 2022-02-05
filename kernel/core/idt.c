///-----------------------------------------------------------------------------
///
/// @file    idt.c
/// @brief   Implementation of IDT related functions
/// @details
///
///   The Interrupt Descriptor Table (IDT) telling the CPU where the Interrupt
///   Service Routines (ISR) are located (one per interrupt vector). The IDT
///   entries are called gates. It can contain Interrupt Gates, Task Gates and
///   Trap Gates. As the first step, only trap gates (exceptions) are
///   implemented.
///
///   Ref: https://wiki.osdev.org/Interrupt_Descriptor_Table
///
/// @author  JW
/// @date    Nov 27, 2021
///
///-----------------------------------------------------------------------------
#include <stdint.h>

#include <lib/klog.h>
#include <core/isr_base.h>
#include <core/idt.h>
#include <core/cpu.h>
#include <core/panic.h>

static idt_entry_t idt[IDT_ENTRIES];
static uint8_t available_vector = 80;

static idt_entry_t idt_make_entry(uint64_t offset)
{
    return (idt_entry_t) {
        .selector = 0x08,
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

void idt_init()
{
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

    idt_register_t idt_register = {
            .size = sizeof(idt_entry_t) * IDT_ENTRIES - 1,
            .offset = (uint64_t)idt };
    asm volatile ("lidt %0" : : "m"(idt_register));
    isr_enable_interrupts();

    klogi("IDT initialization finished\n");
}

