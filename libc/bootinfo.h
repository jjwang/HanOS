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
/* Value used for the end-of-file key (Ctrl-D). Matches the kernel's EOF. */
#define INPUT_KEY_EOF       0xFF
/* Message tag used by a server to write a byte to the kernel console. */
#define CONSOLE_WRITE_TAG   0x50

typedef struct {
    uint64_t magic;
    uint64_t irq_ep;            /* endpoint handle: IRQ notifications in */
    uint64_t key_ep;            /* endpoint handle: decoded keys out */
    uint64_t console_ep;        /* endpoint handle: console bytes out */
    uint64_t irq_num;
    struct {
        uint16_t first;
        uint16_t last;
    } io_ports[BOOTINFO_MAX_IO_RANGES];
    uint64_t io_port_count;

    /* Framebuffer granted to the console server. */
    uint64_t fb_vaddr;
    uint64_t fb_width;
    uint64_t fb_height;
    uint64_t fb_pitch;
    uint64_t fb_size;
    uint64_t cursor_x;
    uint64_t cursor_y;
    uint64_t fgcolor;
    uint64_t bgcolor;
} bootinfo_t;
