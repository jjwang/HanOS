/**-----------------------------------------------------------------------------

 @file    acpi.c
 @brief   Implementation of ACPI (Advanced Configuration and Power Management
          Interface) functions
 @details
 @verbatim

  This module includes implementation of RSDT/XSDT initialization and
  "MADT/HPET" parsing.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include <string.h>

#include <arch/x64/acpi.h>
#include <arch/x64/madt.h>
#include <mm/mm.h>
#include <lib/klog.h>

static acpi_sdt_t *sdt = NULL;
static bool use_xsdt = false;

/* ACPI tables live in firmware-reserved memory, so the HHDM does not cover
 * them. Map the pages of a table into the kernel direct map before reading it. */
static void acpi_map(uint64_t phys, uint64_t len)
{
    if (len < sizeof(acpi_sdt_hdr_t))
        len = sizeof(acpi_sdt_hdr_t);
    vmm_map(NULL, PHYS_TO_VIRT(phys), phys, NUM_PAGES(len), VMM_FLAGS_DEFAULT);
}

acpi_sdt_t *acpi_get_sdt(const char *sign)
{
    uint64_t len = (sdt->hdr.length
                    - sizeof(acpi_sdt_hdr_t)) / (use_xsdt ? 8 : 4);

    for (uint64_t i = 0; i < len; i++) {
        uint64_t phys = use_xsdt
            ? ((uint64_t *) sdt->data)[i] : ((uint32_t *) sdt->data)[i];

        acpi_map(phys, sizeof(acpi_sdt_hdr_t));

        acpi_sdt_t *table = (acpi_sdt_t *) PHYS_TO_VIRT(phys);

        if (memcmp(table->hdr.sign, sign, strlen(sign)) == 0) {
            acpi_map(phys, table->hdr.length);
            klogi("ACPI: found SDT \"%s\" 0x%016lx\n", sign, table);
            return table;
        }
    }

    klogw("ACPI: SDT \"%s\" not found\n", sign);
    return NULL;
}

void acpi_init(struct limine_rsdp_response *rsdp_info)
{
    /* RSDP (Root System Description Pointer) is a data structure used in the
     * ACPI programming interface.
     */
    rsdp_t *rsdp = (rsdp_t *) rsdp_info->address;

    /* The ACPI Version can be detected using the Revision field in the RSDP.
     * If this field contains 0, then ACPI Version 1.0 is used. For subsequent
     * versions (ACPI version 2.0 to 6.1), the value 2 is used
     */
    uint64_t sdt_phys;

    if (rsdp->revision == 2) {
        klogi("ACPI: v2.0 detected\n");
        sdt_phys = rsdp->xsdt_addr;
        use_xsdt = true;
    } else {
        klogi("ACPI: v1.0 (revision %ld) detected\n", rsdp->revision);
        sdt_phys = rsdp->rsdt_addr;
        use_xsdt = false;
    }

    acpi_map(sdt_phys, sizeof(acpi_sdt_hdr_t));
    sdt = (acpi_sdt_t *) PHYS_TO_VIRT(sdt_phys);
    acpi_map(sdt_phys, sdt->hdr.length);

    madt_init();
}
