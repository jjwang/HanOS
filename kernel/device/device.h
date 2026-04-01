/**-----------------------------------------------------------------------------

 @file    device.h
 @brief   Generic device interface for HanOS device drivers
 @details
 @verbatim

  Defines the base device interface shared by all device types. Specialized
  interfaces (block_device.h, char_device.h) embed device_info_t as their
  first member so they can be safely cast to device_info_t *.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    DEVICE_TYPE_BLOCK,
    DEVICE_TYPE_CHAR,
} device_type_t;

typedef struct {
    char           name[32];        /* human-readable device name */
    device_type_t  type;
    int64_t      (*ioctl)(void *ctx, int64_t request, int64_t arg);
    void         (*dump_info)(void *ctx);
    void          *ctx;             /* driver-private context pointer */
} device_info_t;
