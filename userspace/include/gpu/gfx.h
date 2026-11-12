/**-----------------------------------------------------------------------------

 @file    gfx.h
 @brief   Graphics device definitions for the userspace GPU server
 @details
 @verbatim

  The kernel grants the GTTMMADR and GMADR BARs and the boot mode. This header
  describes the GTT, the GPU objects the server allocates, and the MMIO
  accessors used by the display driver.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define GPU_PAGE_SIZE       4096
#define GPU_PAGE_SHIFT      12

#include <gpu_types.h>

#ifndef PAGE_ALIGN_UP
#define PAGE_ALIGN_UP(x) (((x) + GPU_PAGE_SIZE - 1) & ~(uint64_t)(GPU_PAGE_SIZE - 1))
#endif
#define GPU_NUM_PAGES(x) (((x) + GPU_PAGE_SIZE - 1) / GPU_PAGE_SIZE)

static inline uint32_t gpu_mmio_ind(volatile void *p)
{
    return *(volatile uint32_t *)p;
}

static inline void gpu_mmio_outd(volatile void *p, uint32_t v)
{
    *(volatile uint32_t *)p = v;
}

#define gfx_ind(pci, reg) \
    gpu_mmio_ind((volatile uint8_t *)(pci)->mmio_bar + (reg))
#define gfx_outd(pci, reg, val) \
    gpu_mmio_outd((volatile uint8_t *)(pci)->mmio_bar + (reg), (val))
#define gfx_inl(pci, reg) \
    (*(volatile uint32_t *)((volatile uint8_t *)(pci)->mmio_bar + (reg)))
#define gfx_outl(pci, reg, val) \
    (*(volatile uint32_t *)((volatile uint8_t *)(pci)->mmio_bar + (reg)) = (val))

bool gfx_alloc(gfx_mem_manager_t *mgr, gfx_gtt_t *gtt, gfx_object_t *obj,
               uint64_t size, uint64_t align);
uint32_t gfx_gtt_map(gfx_gtt_t *gtt, uint64_t gpu_addr, uint64_t phys,
                     uint64_t size);
void gfx_gtt_write_entry(gfx_gtt_t *gtt, uint32_t index, uint64_t phys);

bool gfx_fb_alloc(uint32_t width, uint32_t height, gfx_fb_t *out_fb);
void gfx_fb_free(gfx_fb_t *fb);
void gpu_flush(const void *addr, uint64_t len);
bool gfx_wait_vblank_frame(void);
uint32_t gfx_pipe_frame(void);
