/**-----------------------------------------------------------------------------

 @file    idt.h
 @brief   Definition of IDT related data structures
 @details
 @verbatim

  The Interrupt Descriptor Table (IDT) telling the CPU where the Interrupt
  Service Routines (ISR) are located (one per interrupt vector).

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdint.h>

#define IDT_ENTRIES                     256
#define IDT_DEFAULT_TYPE_ATTRIBUTES     0b10001110

typedef struct [[gnu::packed]] {
    uint16_t offset_1;        /* offset bits 0..15 */
    uint16_t selector;        /* a code segment selector in GDT or LDT */
    uint8_t  ist;             /* bits 0..2 holds Interrupt Stack Table offset, rest of bits zero. */
    uint8_t  type_attributes; /* gate type, dpl, and p fields */
    uint16_t offset_2;        /* offset bits 16..31 */
    uint32_t offset_3;        /* offset bits 32..63 */
    uint32_t zero;            /* reserved */
} idt_entry_t;

typedef struct [[gnu::packed]] {
    uint16_t size;
    uint64_t offset;
} idt_register_t;

void idt_init();
void idt_set_handler(uint8_t vector, void* handler);
uint8_t idt_get_available_vector(void);
void irq_set_mask(uint8_t line);
void irq_clear_mask(uint8_t line);

