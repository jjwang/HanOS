///-----------------------------------------------------------------------------
///
/// @file    apic.c
/// @brief   Implementation of APIC (Advanced Programmable Interrupt Controller)
///          functions
/// @details
///
///   Need further work on APIC...
///
/// @author  JW
/// @date    DEC 12, 2021
///
///-----------------------------------------------------------------------------
#include <core/acpi.h>
#include <core/apic.h>
#include <core/madt.h>
#include <core/mm.h>
#include <core/cpu.h>
#include <core/idt.h>
#include <lib/klog.h>

static void* lapic_base;

uint32_t apic_read_reg(uint16_t offset)
{
    return *(volatile uint32_t*)(lapic_base + offset);
}

void apic_write_reg(uint16_t offset, uint32_t val)
{
    *(volatile uint32_t*)(lapic_base + offset) = val;
}

void apic_send_eoi()
{
    apic_write_reg(APIC_REG_EOI, 1);
}

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
    lapic_base = (void*)PHYS_TO_VIRT(madt_get_lapic_base());
    vmm_map((uint64_t)lapic_base, VIRT_TO_PHYS(lapic_base), 1, VMM_FLAGS_MMIO);

    apic_enable();

    klog_printf("APIC initialization finished.\n");
}
