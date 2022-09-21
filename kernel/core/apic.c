/**-----------------------------------------------------------------------------

 @file    apic.c
 @brief   Implementation of APIC (Advanced Programmable Interrupt Controller)
          functions
 @details
 @verbatim

  APIC ("Advanced Programmable Interrupt Controller") is the updated Intel
  standard for the older PIC. It is used in multiprocessor systems and is
  an integral part of all recent Intel (and compatible) processors. The
  APIC is used for sophisticated interrupt redirection, and for sending
  interrupts between processors.

  Ref: https://wiki.osdev.org/APIC

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <core/acpi.h>
#include <core/apic.h>
#include <core/madt.h>
#include <core/mm.h>
#include <core/cpu.h>
#include <core/idt.h>
#include <core/panic.h>
#include <lib/klog.h>

/* The local APIC registers are memory mapped to an address that can be found
 * in the MP/MADT tables.
 */
static void* lapic_base = NULL;

uint32_t apic_read_reg(uint16_t offset)
{
    return *(volatile uint32_t*)(lapic_base + offset);
}

void apic_write_reg(uint16_t offset, uint32_t val)
{
    *(volatile uint32_t*)(lapic_base + offset) = val;
}

/* Write to the register with offset 0xB0 using the value 0 to signal an end
 * of interrupt.
 */
void apic_send_eoi()
{
    apic_write_reg(APIC_REG_EOI, 1);
}

/* Inter-Processor Interrupts (IPIs) can be used as basic signaling for
 * scheduling coordination, multi-processor bootstrapping, etc.
 */
void apic_send_ipi(uint8_t dest, uint8_t vector, uint32_t mtype)
{
    apic_write_reg(APIC_REG_ICR_HIGH, (uint32_t)dest << 24);
    apic_write_reg(APIC_REG_ICR_LOW, (mtype << 8) | vector);
}

void apic_enable()
{
    apic_write_reg(APIC_REG_SPURIOUS_INT, APIC_FLAG_ENABLE | APIC_SPURIOUS_VECTOR_NUM);
}

void apic_init()
{
    /* QEMU does not support APIC virtualization if host CPU cannot support. */
    if (cpuid_check_feature(CPUID_FEATURE_APIC)) {
        kloge("APIC: unsupported indicated by CPU flag\n");
    }

    lapic_base = (void*)PHYS_TO_KER(madt_get_lapic_base());
    vmm_map(NULL, (uint64_t)lapic_base, KER_TO_PHYS(lapic_base), 1, VMM_FLAGS_MMIO);

    apic_enable();

    klogi("APIC version %08x initialization finished\n", apic_read_reg(APIC_REG_VERSION));
}

