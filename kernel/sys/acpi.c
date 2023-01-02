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
#include <stdbool.h>
#include <stddef.h>
#include <sys/acpi.h>
#include <sys/madt.h>
#include <sys/mm.h>
#include <lib/klog.h>
#include <lib/memutils.h>
#include <lib/string.h>

static acpi_sdt_t* sdt = NULL;
static bool use_xsdt = false;

acpi_sdt_t* acpi_get_sdt(const char* sign)
{
    uint64_t len = (sdt->hdr.length
            - sizeof(acpi_sdt_hdr_t)) / (use_xsdt ? 8 : 4);

    for (size_t i = 0; i < len; i++) {
        acpi_sdt_t* table = (acpi_sdt_t*)PHYS_TO_VIRT(
                   (use_xsdt ? ((uint64_t*)sdt->data)[i] : ((uint32_t*)sdt->data)[i]));
        if (memcmp(table->hdr.sign, sign, strlen(sign))) {
            klogi("ACPI: found SDT \"%s\" 0x%x\n", sign, table);
            return table;
        }
    }

    klogw("ACPI: SDT \"%s\" not found\n", sign);
    return NULL;
}

void acpi_init(struct limine_rsdp_response* rsdp_info)
{
    /* RSDP (Root System Description Pointer) is a data structure used in the
     * ACPI programming interface.
     */
    rsdp_t* rsdp = (rsdp_t*)rsdp_info->address;

    /* The ACPI Version can be detected using the Revision field in the RSDP.
     * If this field contains 0, then ACPI Version 1.0 is used. For subsequent
     * versions (ACPI version 2.0 to 6.1), the value 2 is used
     */
    if (rsdp->revision == 2) {
        klogi("ACPI: v2.0 detected\n");
        sdt = (acpi_sdt_t*)PHYS_TO_VIRT(rsdp->xsdt_addr);
        use_xsdt = true;
    } else {
        klogi("ACPI: v1.0 (revision %d) detected\n", rsdp->revision);
        sdt = (acpi_sdt_t*)PHYS_TO_VIRT(rsdp->rsdt_addr);
        use_xsdt = false;
    }

    madt_init();
}
