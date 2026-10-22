/**-----------------------------------------------------------------------------

 @file    edid_read.h
 @brief   EDID source readers

 @details
 @verbatim

  Declares the EDID readers used by the graphics init path. The format
  definitions stay in edid.h so display_mode.h does not pull in PCI types.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <arch/x64/pci.h>
#include <device/display/gfx.h>
#include <device/display/edid.h>

/* Validate a 128-byte EDID block: header, checksum and first DTD clock. */
bool edid_validate(const uint8_t * buf);

/* Read the panel EDID from ACPI OpRegion ASLE mailbox #5. */
bool edid_read_opregion(pci_device_t dev, uint8_t * buf, uint32_t len);

/* Power the eDP panel/VDD, read EDID over AUX and validate it. */
bool edid_read_aux(gfx_pci_t * pci, uint8_t * buf, uint32_t len);
