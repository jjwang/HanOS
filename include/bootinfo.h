/**-----------------------------------------------------------------------------

 @file    bootinfo.h
 @brief   Startup information passed to a userspace server
 @details
 @verbatim

   The kernel spawns the first userspace servers with a small bootinfo block
   describing the resources it granted them (endpoint handles in the server's
   own handle table, the interrupt line, and I/O-port ranges). A server reads
   it with sys_bootinfo(). The same header also defines the input message tags
   shared by the input server and the kernel.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdint.h>

#define BOOTINFO_MAGIC      0x48414e4f53424b30ULL      /* "HANOSBK0" */

#define BOOTINFO_MAX_IO_RANGES  4

/* Message tag used when a bound interrupt line fires. */
#define IRQ_NOTIFY_TAG      0x01
/* Message tag used by the input server to hand a decoded key to the kernel. */
#define INPUT_KEY_TAG       0x40
/* Message tag used by the input server to hand a mouse delta (words[0]=dx,
 * words[1]=dy) to the kernel. */
#define INPUT_MOUSE_TAG     0x41
/* Value used for the end-of-file key (Ctrl-D). Matches the kernel's EOF. */
#define INPUT_KEY_EOF       0xFF
/* Message tag used by a server to write a byte to the kernel console. */
#define CONSOLE_WRITE_TAG   0x50

/**
 * @brief An inclusive range of I/O ports granted to a server
 */
typedef struct {
    uint16_t first;             /**< First port in the range. */
    uint16_t last;              /**< Last port in the range. */
} io_port_range_t;

/**
 * @brief Startup resources granted to a userspace server
 */
typedef struct {
    uint64_t magic;             /**< BOOTINFO_MAGIC. */
    uint64_t irq_ep;            /**< Endpoint handle: IRQ notifications in. */
    uint64_t key_ep;            /**< Endpoint handle: decoded keys out. */
    uint64_t console_ep;        /**< Endpoint handle: console bytes out. */
    uint64_t service_ep;        /**< Endpoint handle: this server's requests in. */
    uint64_t block_ep;          /**< Endpoint handle: block server requests out. */
    uint64_t fs_ep;             /**< Process server: VFS requests out. */
    uint64_t pipe_ep;           /**< Process server: pipe requests out. */
    uint64_t tty_ep;            /**< Process server: tty requests out. */
    uint64_t fat_ep;            /**< Process server: FAT requests out. */
    uint64_t ext2_ep;           /**< VFS server: ext2 requests out. */
    uint64_t irq_num;           /**< Granted interrupt line. */
    io_port_range_t io_ports[BOOTINFO_MAX_IO_RANGES];  /**< Granted I/O ranges. */
    uint64_t io_port_count;     /**< Number of valid io_ports entries. */

    /* Framebuffer granted to the console server. */
    uint64_t fb_vaddr;          /**< Framebuffer virtual address. */
    uint64_t fb_width;          /**< Framebuffer width in pixels. */
    uint64_t fb_height;         /**< Framebuffer height in pixels. */
    uint64_t fb_pitch;          /**< Framebuffer pitch in bytes. */
    uint64_t fb_size;           /**< Framebuffer size in bytes. */
    uint64_t desktop;           /**< Framebuffer is the desktop console sprite. */
    uint64_t cursor_x;          /**< Initial text cursor column. */
    uint64_t cursor_y;          /**< Initial text cursor row. */
    uint64_t fgcolor;           /**< Default foreground colour. */
    uint64_t bgcolor;           /**< Default background colour. */

    /* Initrd (ustar archive) granted to the VFS server, mapped read-only. */
    uint64_t initrd_vaddr;      /**< Initrd virtual address. */
    uint64_t initrd_size;       /**< Initrd size in bytes. */

    /* NIC MMIO window granted to the network server. */
    uint64_t net_mmio_vaddr;    /**< NIC MMIO virtual address. */
    uint64_t net_mmio_size;     /**< NIC MMIO size in bytes. */

    /* Physically contiguous DMA region granted to the network server. */
    uint64_t net_dma_vaddr;     /**< NIC DMA virtual address. */
    uint64_t net_dma_phys;      /**< NIC DMA physical address. */
    uint64_t net_dma_size;      /**< NIC DMA size in bytes. */

    /* AHCI ABAR and DMA region granted to the block server. */
    uint64_t block_mmio_vaddr;  /**< AHCI ABAR virtual address. */
    uint64_t block_mmio_size;   /**< AHCI ABAR size in bytes. */
    uint64_t block_dma_vaddr;   /**< AHCI DMA virtual address. */
    uint64_t block_dma_phys;    /**< AHCI DMA physical address. */
    uint64_t block_dma_size;    /**< AHCI DMA size in bytes. */

    /* HPET MMIO window and boot wall-clock granted to the misc server. */
    uint64_t misc_hpet_vaddr;   /**< HPET MMIO virtual address. */
    uint64_t misc_hpet_period;  /**< Nanoseconds per HPET tick. */
    uint64_t misc_hpet_base;    /**< HPET counter at hpet_init. */
    uint64_t misc_boot_time;    /**< Boot wall-clock in seconds. */
    char misc_version[32];      /**< HanOS version string. */
} bootinfo_t;
