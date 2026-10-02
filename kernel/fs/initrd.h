/**-----------------------------------------------------------------------------

 @file    initrd.h
 @brief   Read files from the boot initrd image

 @details
 @verbatim

   The bootloader hands the kernel a ustar archive. This module records it and
   looks a file up by path. It replaces the in-kernel filesystem the kernel
   used to build: only the first userspace servers are loaded from here, before
   the VFS server can serve paths.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdint.h>

/* Record the boot initrd archive. */
void initrd_init(void *address, uint64_t size);

/* Address and length of the boot initrd, for the VFS server. */
void initrd_get(void **address, uint64_t *size);

/* Read a file from the initrd into a freshly kmalloc_chunk()'d buffer. The
 * path may be absolute or relative. Returns 0 on success. */
int64_t initrd_load(const char *path, uint8_t **out_buf, uint64_t *out_len);
