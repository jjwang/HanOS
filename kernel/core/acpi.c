///-----------------------------------------------------------------------------
///
/// @file    acpi.c
/// @brief   Implementation of ACPI (Advanced Configuration and Power Management
///          Interface) functions
/// @details
///
///   This module includes implementation of RSDT/XSDT initialization and
///   "MADT/HPET" parsing.
///
/// @author  JW
/// @date    DEC 12, 2021
///
///-----------------------------------------------------------------------------
#include <stdbool.h>
#include <stddef.h>
#include <core/acpi.h>
#include <core/madt.h>
#include <core/mm.h>
#include <lib/klog.h>
#include <lib/memutils.h>

static acpi_sdt_t* sdt = NULL;
static bool is_xsdt = false;

acpi_sdt_t* acpi_get_sdt(const char* sign)
{
    uint64_t len = (sdt->hdr.length - sizeof(acpi_sdt_t))
                    /
                   (is_xsdt ? sizeof(uint64_t) : sizeof(uint32_t));
    for (uint64_t i = 0; i < len; i++) {
        acpi_sdt_t* table = (acpi_sdt_t*)PHYS_TO_VIRT(
                   (is_xsdt ? ((uint64_t*)sdt->data)[i] : ((uint32_t*)sdt->data)[i]));
        if (memcmp(table->hdr.sign, sign, sizeof(table->hdr.sign))) {
            klogi("ACPI: found SDT \"%s\"\n", sign);
            return table;
        }
    }

    klogw("ACPI: SDT \"%s\" not found\n", sign);
    return NULL;
}

void acpi_init(struct stivale2_struct_tag_rsdp* rsdp_info)
{
    // RSDP (Root System Description Pointer) is a data structure used in the
    // ACPI programming interface.
    rsdp_t* rsdp = (rsdp_t*)PHYS_TO_VIRT(rsdp_info->rsdp);

    // The ACPI Version can be detected using the Revision field in the RSDP.
    // If this field contains 0, then ACPI Version 1.0 is used. For subsequent
    // versions (ACPI version 2.0 to 6.1), the value 2 is used
    if (rsdp->revision == 2) {
        klogi("ACPI: v2.0 detected\n");
        sdt = (acpi_sdt_t*)PHYS_TO_VIRT(rsdp->xsdt_addr);
        is_xsdt = true;
    } else {
        klogi("ACPI: v1.0 (revision %d) detected\n", rsdp->revision);
        sdt = (acpi_sdt_t*)PHYS_TO_VIRT(rsdp->rsdt_addr);
        is_xsdt = false;
    }

    madt_init();
}
