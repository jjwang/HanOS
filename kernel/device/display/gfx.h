/**-----------------------------------------------------------------------------

 @file    gfx.h
 @brief   Early eDP bootstrap definitions

 @details
 @verbatim

  The kernel keeps only the GTT, framebuffer and mode-set definitions needed
  to switch the panel before the HNK splash. The userspace GPU server owns the
  runtime planes, desktop and cursor.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <arch/x64/pci.h>
#include <gpu_types.h>

bool gfx_boot_device(pci_device_t *out);
bool gfx_boot_is_gen9(uint16_t id);
bool gfx_boot_has_64bit_gtt(uint16_t id);
bool gfx_init(void);
bool gfx_early_active(void);
bool gfx_alloc(gfx_mem_manager_t *mgr, gfx_gtt_t *gtt, gfx_object_t *obj,
               uint64_t size, uint64_t align);
void gfx_gtt_write_entry(gfx_gtt_t *gtt, uint32_t index, uint64_t phys);

#define gfx_ind(pci, reg)           mmio_ind((uint8_t*)(pci)->mmio_bar + (reg))
#define gfx_outd(pci, reg, val)     mmio_outd((uint8_t*)(pci)->mmio_bar + (reg), val)
#define gfx_inl(pci, reg)           mmio_inl((uint8_t*)(pci)->mmio_bar + (reg))
#define gfx_outl(pci, reg, val)     mmio_outl((uint8_t*)(pci)->mmio_bar + (reg), val)
