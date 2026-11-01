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

#include <stdbool.h>
#include <stdint.h>

#include <arch/x64/pci.h>

/**
 * @brief Graphics PCI device resources
 */
typedef struct {
    uint32_t id;                /**< PCI id: bus, device and function. */
    uint16_t device_id;         /**< PCI device id. */
    bool gen9;                  /**< Skylake display path. */
    bool pte64;                 /**< 64-bit GTT entries (Gen8+). */

    uint32_t gtt_offset;        /**< GTT offset inside the MMIO BAR. */
    uint32_t gtt_size;          /**< GTT window size. */

    volatile void *aperture_bar;        /**< GMADR aperture base. */
    volatile void *mmio_bar;    /**< GTTMMADR MMIO base. */
    volatile uint64_t *gtt_addr;        /**< GTT entry array. */
    uint16_t iobase;            /**< I/O BAR base. */

    uint32_t aperture_size;     /**< Aperture size in bytes. */
} gfx_pci_t;

/**
 * @brief Graphics translation table (GTT) state
 */
typedef struct {
    uint32_t stolen_mem_size;   /**< Stolen memory size. */
    uint32_t gtt_mem_size;      /**< GTT window size. */
    uint32_t stolen_mem_base;   /**< Stolen memory base. */

    uint32_t num_total_entries; /**< GTT entry count. */
    uint32_t num_mappable_entries;      /**< Mappable entry count. */
    bool pte64;                 /**< 64-bit GTT entries (Gen8+). */

    volatile uint64_t *entries; /**< GTT entry array. */
} gfx_gtt_t;

/**
 * @brief A GPU object mapped through the GTT
 */
typedef struct {
    volatile uint8_t *cpu_addr; /**< CPU direct-map address. */
    volatile uint64_t gfx_addr; /**< GPU address. */
    uint64_t phys;              /**< Physical base of the contiguous allocation. */
    uint64_t size;              /**< Allocation size in bytes. */
    uint32_t pages;             /**< Page count for pmm_free. */
} gfx_object_t;

/**
 * @brief A range of GPU-addressable memory
 */
typedef struct {
    uint64_t base;              /**< Range base. */
    uint64_t top;               /**< Range top. */
    uint64_t current;           /**< Next free address. */
} gfx_mem_range_t;

/**
 * @brief Graphics memory allocator state
 */
typedef struct {
    gfx_mem_range_t vram;       /**< Stolen memory. */
    gfx_mem_range_t shared;     /**< Aperture-mapped addresses. */
    gfx_mem_range_t priv;       /**< GPU-only, CPU-allocated addresses. */

    volatile uint8_t *gfx_mem_base;     /**< Memory base. */
    volatile uint8_t *gfx_mem_next;     /**< Next free byte. */
} gfx_mem_manager_t;

/**
 * @brief GPU performance and frequency status
 */
typedef struct {
    uint32_t current_freq_mhz;  /**< Current GPU frequency. */
    uint32_t requested_freq_mhz;        /**< Requested GPU frequency. */
    uint32_t perf_limit_reasons;        /**< Performance limit reasons. */
    uint8_t rp0_freq_units;     /**< RP0 cap in 50 MHz units. */
} gfx_perf_status_t;

/**
 * @brief A framebuffer presented on a display plane
 */
typedef struct {
    gfx_object_t obj;           /**< GTT-mapped framebuffer. */
    uint32_t width;             /**< Width in pixels. */
    uint32_t height;            /**< Height in pixels. */
    uint32_t stride;            /**< Bytes per row. */
    uint32_t format;            /**< DISPPLANE_BGRX888 and similar. */
    uint64_t *pages;            /**< Scatter GTT page list, or NULL. */
    uint32_t page_count;        /**< Scatter page count. */
} gfx_fb_t;

/** @brief Graphics fence register base. */
#define FENCE_BASE                      0x100000
/** @brief Number of graphics fence registers. */
#define FENCE_COUNT                     16

bool pci_get_gfx_device(pci_device_t * gfx_dev);

bool gfx_init(void);
void gfx_start(void);

/* Move the hardware cursor by a relative amount. */
void gfx_cursor_move(int32_t dx, int32_t dy);

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

/* Double buffering and vblank */
/** @brief Allocate a contiguous GTT framebuffer.
 * @param width Width in pixels.
 * @param height Height in pixels.
 * @param out_fb Output framebuffer.
 * @return true on success. */
bool gfx_fb_alloc(uint32_t width, uint32_t height, gfx_fb_t * out_fb);
/** @brief Free a GTT framebuffer.
 * @param fb Framebuffer to free. */
void gfx_fb_free(gfx_fb_t * fb);
/** @brief Point plane 1 at a framebuffer.
 * @param fb New scan-out surface. */
void gfx_flip_to(const gfx_fb_t * fb);
/** @brief Wait for one frame boundary.
 * @return true on a frame, false on timeout. */
bool gfx_wait_vblank_frame(void);
/** @brief Route and enable the vblank interrupt.
 * @return true when the line is armed. */
bool gfx_vblank_irq_start(void);
/** @brief Run the double-buffer flip self-test.
 * @return true when every frame is presented. */
bool gfx_flip_test(void);
/** @brief Run the three-layer composition demo.
 * @param mode 0 full, 1 primary rain, 2 solid bands.
 * @return true when the demo completes. */
bool gfx_layers_demo(int mode);
/** @brief Bring up the persistent scrolled-rain desktop.
 * @return true when the desktop owns the display. */
bool gfx_desktop_start(void);
/** @brief Report whether the desktop owns the display.
 * @return true when active. */
bool gfx_desktop_active(void);
/** @brief Return the console sprite surface.
 * @return The sprite, or NULL when the desktop is inactive. */
gfx_fb_t *gfx_desktop_console_fb(void);
/** @brief Return the vblank interrupt count.
 * @return Frames counted by the handler. */
uint64_t gfx_vblank_count(void);
/** @brief Report whether the vblank interrupt is armed.
 * @return true when armed. */
bool gfx_vblank_irq_active(void);
/** @brief Return the routed GPU interrupt line.
 * @return PCI interrupt line, or 0. */
uint8_t gfx_vblank_irq_line(void);
/** @brief Read the display pipe interrupt identity.
 * @return IIR value. */
uint32_t gfx_display_iir(void);
/** @brief Read the display pipe interrupt enable.
 * @return IER value. */
uint32_t gfx_display_ier(void);
/** @brief Read the display pipe interrupt mask.
 * @return IMR value. */
uint32_t gfx_display_imr(void);
/** @brief Read the master display interrupt register.
 * @return GEN8_MASTER_IRQ value. */
uint32_t gfx_master_irq(void);
/** @brief Read the pipe A frame counter.
 * @return Frame counter value. */
uint32_t gfx_pipe_frame(void);
/** @brief Read the frame counter after a delay.
 * @param ms Delay in milliseconds.
 * @return Frame counter value. */
uint32_t gfx_pipe_frame_after(uint32_t ms);
/** @brief Read the I/O APIC route for the vblank line.
 * @return Low and high redirection entry, or 0. */
uint64_t gfx_vblank_route(void);

/* eDP panel and mode setting */
bool gfx_edp_panel_on(gfx_pci_t * pci);
bool gfx_edp_panel_off(gfx_pci_t * pci);
bool gfx_modeset(gfx_pci_t * pci, gfx_mem_manager_t * mgr, gfx_gtt_t * gtt,
                 uint32_t width, uint32_t height, uint32_t pitch,
                 uint32_t format, gfx_fb_t * out_fb);
