/**-----------------------------------------------------------------------------

 @file    char_device.h
 @brief   Character device interface for HanOS drivers
 @details
 @verbatim

  Specializes device_info_t for character (byte-stream) devices such as
  keyboards, serial ports, and terminals. The base field is first so a
  char_device_ops_t * can be cast to device_info_t *.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <device/device.h>

typedef struct {
    device_info_t  base;            /* must be first — cast-compatible with device_info_t */
    int64_t (*read) (void *ctx, uint64_t len, void *buf);
    int64_t (*write)(void *ctx, uint64_t len, const void *buf);
    bool    (*poll) (void *ctx);    /* returns true if data is available to read */
} char_device_ops_t;
