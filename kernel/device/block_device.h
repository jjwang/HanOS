/**-----------------------------------------------------------------------------

 @file    block_device.h
 @brief   Block device interface for HanOS storage drivers
 @details
 @verbatim

  Specializes device_info_t for block (sector-addressable) devices. The base
  field is first so a block_device_ops_t * can be cast to device_info_t *.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdint.h>
#include <device/device.h>

typedef struct {
    device_info_t  base;            /* must be first — cast-compatible with device_info_t */
    void     (*read) (void *ctx, uint32_t lba, uint8_t sector_count, uint8_t *buf);
    void     (*write)(void *ctx, uint32_t lba, uint8_t sector_count, uint8_t *buf);
    uint32_t (*get_sector_size) (void *ctx);
    uint32_t (*get_sector_count)(void *ctx);
} block_device_ops_t;
