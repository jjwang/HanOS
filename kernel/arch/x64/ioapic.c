/**-----------------------------------------------------------------------------

 @file    ioapic.c
 @brief   Minimal I/O APIC redirection-table programming
 @details
 @verbatim

   Enabling the local APIC routes external interrupts through the I/O APIC on
   real hardware. The kernel programs only the redirection entries the legacy
   ISA lines use (timer, keyboard, mouse, serial). Each entry delivers the
   PIC-compatible vector (0x20 + IRQ) to the boot processor's local APIC.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdbool.h>
#include <stdint.h>

#include <arch/x64/ioapic.h>
#include <arch/x64/madt.h>
#include <lib/klog.h>
#include <mm/mm.h>

#define IOAPIC_IOREGSEL     0x00
#define IOAPIC_IOWIN        0x10
#define IOAPIC_REDTBL       0x10

static volatile uint32_t *ioapic = NULL;
static uint32_t gsi_base = 0;

static void ioapic_write(uint32_t reg, uint32_t val)
{
    ioapic[IOAPIC_IOREGSEL / 4] = reg;
    ioapic[IOAPIC_IOWIN / 4] = val;
}

void ioapic_init(void)
{
    if (ioapic != NULL || madt_get_num_ioapic() == 0)
        return;

    madt_record_ioapic_t *rec = madt_get_ioapics()[0];

    if (rec->addr == 0)
        return;

    gsi_base = rec->gsi_base;
    ioapic = (volatile uint32_t *) PHYS_TO_VIRT(rec->addr);
    vmm_map(NULL, (uint64_t) ioapic, rec->addr, 1, VMM_FLAGS_MMIO);
    klogi("IOAPIC: base 0x%08x gsi_base %u\n", rec->addr, gsi_base);
}

bool ioapic_available(void)
{
    return ioapic != NULL;
}

void ioapic_set_line(uint8_t irq, bool masked)
{
    if (!ioapic_available())
        return;

    /* IRQ2 is the 8259 cascade, not a device line. Its GSI may even belong to
     * another line through an interrupt source override. */
    if (irq == 2)
        return;

    uint32_t gsi;
    uint16_t flags;

    madt_isa_to_gsi(irq, &gsi, &flags);

    if (gsi < gsi_base)
        return;

    uint32_t idx = gsi - gsi_base;
    uint32_t low = (uint32_t) (0x20 + irq);     /* PIC-compatible vector */

    /* ISO flags: bits 0-1 polarity (11 = active low), bits 2-3 trigger
     * (11 = level). ISA defaults stay edge, active high. */
    if ((flags & 0x3) == 0x3)
        low |= 1u << 13;
    if ((flags & 0xc) == 0xc)
        low |= 1u << 15;
    if (masked)
        low |= 1u << 16;

    uint32_t high = (uint32_t) madt_get_bsp_apic_id() << 24;

    ioapic_write(IOAPIC_REDTBL + 2 * idx, low);
    ioapic_write(IOAPIC_REDTBL + 2 * idx + 1, high);
}
