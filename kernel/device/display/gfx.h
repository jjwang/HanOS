/**-----------------------------------------------------------------------------

 @file    gfx.h
 @brief   Graphics device handling definitions
 @details
 @verbatim

  This file contains the type definitions and function declarations for handling
  graphics devices within the HanOS kernel. It includes structures for
  representing PCI graphics devices, graphics translation tables (GTT), graphics
  objects, and memory ranges. Additionally, it provides function prototypes for
  initializing and starting the graphics system, as well as macros for
  interacting with memory-mapped I/O registers of the graphics device.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

typedef struct {
    uint32_t id;

    volatile void *aperture_bar;
    volatile void *mmio_bar;
    volatile uint32_t *gtt_addr;
    uint16_t iobase;

    uint32_t aperture_size;
} gfx_pci_t;

typedef struct {
    uint32_t stolen_mem_size;
    uint32_t gtt_mem_size;
    uint32_t stolen_mem_base;

    uint32_t num_total_entries; /* How many entries in the GTT */
    uint32_t num_mappable_entries;      /* How many can be mapped at once */

    volatile uint32_t *entries;
} gfx_gtt_t;

typedef struct {
    volatile uint8_t *cpu_addr;
    volatile uint64_t gfx_addr;
} gfx_object_t;

typedef struct {
    uint64_t base;
    uint64_t top;
    uint64_t current;
} gfx_mem_range_t;

typedef struct {
    gfx_mem_range_t vram;       /* Stolen Memory */
    gfx_mem_range_t shared;     /* Addresses mapped through aperture. */
    gfx_mem_range_t priv;       /* Only accessable by GPU, but allocated by CPU. */

    volatile uint8_t *gfx_mem_base;
    volatile uint8_t *gfx_mem_next;
} gfx_mem_manager_t;

typedef struct {
    uint32_t current_freq_mhz;
    uint32_t requested_freq_mhz;
    uint32_t perf_limit_reasons;
    uint8_t rp0_freq_units;     /* GT_PERF_STATUS bits 7:0 — RP0 cap in 50MHz units */
} gfx_perf_status_t;

typedef struct {
    gfx_object_t obj;       /* GTT-mapped framebuffer */
    uint32_t width;
    uint32_t height;
    uint32_t stride;        /* bytes per row */
    uint32_t format;        /* DISPPLANE_BGRX888 etc. */
} gfx_fb_t;

#define FENCE_BASE                      0x100000
#define FENCE_COUNT                     16

bool pci_get_gfx_device(pci_device_t * gfx_dev);

bool gfx_init(void);
void gfx_start(void);

/* Testing and diagnostics */
bool gfx_test_advanced_features(gfx_pci_t * pci);
gfx_pci_t *gfx_get_pci(void);

/* Power management */
bool gfx_enter_force_wake(gfx_pci_t * pci);
bool gfx_exit_force_wake(gfx_pci_t * pci);

/* Memory management */
bool gfx_mem_enable_swizzle(gfx_pci_t * pci);
bool gfx_alloc(gfx_mem_manager_t * mgr, gfx_gtt_t * gtt, gfx_object_t * obj,
               uint64_t size, uint64_t align);
uint64_t gfx_addr(gfx_mem_manager_t * mgr, void *phy_addr);

/* GTT mapping */
void gfx_gtt_write_entry(gfx_gtt_t * gtt, uint32_t index, uint64_t phys);
uint32_t gfx_gtt_map(gfx_gtt_t * gtt, uint64_t gpu_addr, uint64_t phys,
                     uint64_t size);
void gfx_gtt_clear(gfx_gtt_t * gtt, uint64_t gpu_addr, uint64_t size);

/* Display control */
void gfx_disable_vga(gfx_pci_t * pci);
bool gfx_is_pipe_enabled(gfx_pci_t * pci, uint8_t pipe);
uint32_t gfx_get_gpu_freq(gfx_pci_t * pci);
void gfx_get_display_info(gfx_pci_t * pci);

/* Pipe and plane control */
bool gfx_wait_pipe_state(gfx_pci_t * pci, uint8_t pipe, bool enabled);
bool gfx_enable_pipe(gfx_pci_t * pci, uint8_t pipe);
bool gfx_disable_pipe(gfx_pci_t * pci, uint8_t pipe);
bool gfx_configure_plane(gfx_pci_t * pci, uint8_t plane, uint32_t format,
                         bool enabled);
bool gfx_set_plane_fb(gfx_pci_t * pci, uint8_t plane, uint64_t addr,
                      uint32_t stride);
bool gfx_set_timing(gfx_pci_t * pci, uint8_t pipe, uint32_t htotal,
                    uint32_t hblank, uint32_t hsync, uint32_t vtotal,
                    uint32_t vblank, uint32_t vsync);

/* Interrupt control */
bool gfx_enable_interrupts(gfx_pci_t * pci, uint32_t mask);
bool gfx_disable_interrupts(gfx_pci_t * pci, uint32_t mask);
uint32_t gfx_get_interrupts(gfx_pci_t * pci);
void gfx_clear_interrupts(gfx_pci_t * pci, uint32_t mask);
bool gfx_wait_vblank(gfx_pci_t * pci, uint8_t pipe);

/* Power and performance */
bool gfx_configure_rc6(gfx_pci_t * pci, bool enable);
bool gfx_set_gpu_freq(gfx_pci_t * pci, uint32_t freq_mhz);
bool gfx_get_perf_status(gfx_pci_t * pci, gfx_perf_status_t * status);

/* Cursor control */
bool gfx_configure_cursor(gfx_pci_t * pci, uint8_t pipe, uint32_t mode,
                          uint64_t addr, int32_t x, int32_t y);
bool gfx_disable_cursor(gfx_pci_t * pci, uint8_t pipe);

#define gfx_ind(pci, reg)           mmio_ind((uint8_t*)(pci)->mmio_bar + (reg))
#define gfx_outd(pci, reg, val)     mmio_outd((uint8_t*)(pci)->mmio_bar + (reg), val)
#define gfx_inl(pci, reg)           mmio_inl((uint8_t*)(pci)->mmio_bar + (reg))
#define gfx_outl(pci, reg, val)     mmio_outl((uint8_t*)(pci)->mmio_bar + (reg), val)

/* eDP panel and mode setting */
bool gfx_edp_panel_on(gfx_pci_t * pci);
bool gfx_edp_panel_off(gfx_pci_t * pci);
bool gfx_modeset(gfx_pci_t * pci, gfx_mem_manager_t * mgr, gfx_gtt_t * gtt,
                 uint32_t width, uint32_t height, uint32_t pitch,
                 uint32_t format, gfx_fb_t * out_fb);
