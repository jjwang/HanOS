/**-----------------------------------------------------------------------------

 @file    vbt.h
 @brief   VBT panel timing reader

 @details
 @verbatim

  Parses the LFP/generic panel timing from the VBT stored in the ACPI
  OpRegion (fixed offset or ASLE.RVDA). This is the i915 fallback when the
  panel EDID is not available.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <arch/x64/pci.h>
#include <device/display/gfx.h>
#include <device/display/display_mode.h>

/* Build a display mode from the VBT panel timing. Returns false when the
 * OpRegion has no usable VBT panel block. */
bool vbt_panel_mode(gfx_pci_t * pci, pci_device_t dev, display_mode_t * mode);
