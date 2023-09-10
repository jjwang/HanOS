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
#include <sys/acpi.h>
#include <sys/apic.h>
#include <sys/madt.h>
#include <sys/mm.h>
#include <sys/cpu.h>
#include <sys/idt.h>
#include <sys/panic.h>
#include <base/klog.h>

/* The local APIC registers are memory mapped to an address that can be found
 * in the MP/MADT tables.
 */
void* lapic_base = NULL;

/**
 * Reads the value of a register from the Advanced Programmable Interrupt
 * Controller (APIC).
 *
 * @param offset The offset of the register to read.
 *
 * @returns The value of the register.
 */
uint32_t apic_read_reg(uint16_t offset)
{
    return *(volatile uint32_t*)(lapic_base + offset);
}

/**
 * Writes a value to a register in the Advanced Programmable Interrupt
 * Controller (APIC).
 *
 * @param offset The offset of the register.
 * @param val The value to write to the register.
 *
 * @returns None
 */
void apic_write_reg(uint16_t offset, uint32_t val)
{
    *(volatile uint32_t*)(lapic_base + offset) = val;
}

/**
 * Sends an End of Interrupt (EOI) signal to the Advanced Programmable Interrupt
 * Controller (APIC).
 *
 * @returns None
 */
void apic_send_eoi()
{
    apic_write_reg(APIC_REG_EOI, 1);
}

/**
 * Sends an Inter-Processor Interrupt (IPI) to a specific destination processor.
 *
 * @param dest The destination processor ID.
 * @param vector The interrupt vector number.
 * @param mtype The message type for the IPI.
 *
 * @returns None
 */
void apic_send_ipi(uint8_t dest, uint8_t vector, uint32_t mtype)
{
    apic_write_reg(APIC_REG_ICR_HIGH, (uint32_t)dest << 24);
    apic_write_reg(APIC_REG_ICR_LOW, (mtype << 8) | vector);
}


/**
 * Enables the Advanced Programmable Interrupt Controller (APIC).
 *
 * This function writes a value to the Spurious Interrupt Vector Register (SVR)
 * of the APIC, enabling it and setting the interrupt vector number to the
 * APIC_SPURIOUS_VECTOR_NUM constant.
 *
 * @returns None
 */
void apic_enable()
{
    apic_write_reg(APIC_REG_SPURIOUS_INT, APIC_FLAG_ENABLE | APIC_SPURIOUS_VECTOR_NUM);
}

/**
 * Initializes the Advanced Programmable Interrupt Controller (APIC).
 * This function checks if the CPU supports APIC, maps the local APIC base
 * address,
 * enables the APIC, and logs the APIC version.
 *
 * @returns None
 */
void apic_init()
{
    /* QEMU does not support APIC virtualization if host CPU cannot support. */
    if (cpuid_check_feature(CPUID_FEATURE_APIC)) {
        kloge("APIC: unsupported indicated by CPU flag\n");
    }

    lapic_base = (void*)PHYS_TO_VIRT(madt_get_lapic_base());

    /* MEMMAP: lapic_base should be visible for all kernel tasks */
    vmm_map(NULL, (uint64_t)lapic_base, VIRT_TO_PHYS(lapic_base), 1,
            VMM_FLAGS_MMIO);

    apic_enable();

    klogi("APIC version %08x initialization finished\n", apic_read_reg(APIC_REG_VERSION));
}

