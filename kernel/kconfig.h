/**-----------------------------------------------------------------------------

 @file    kconfig.h
 @brief   Kernel configuration header
 @details
 @verbatim

  This header file contains configuration macros and type definitions used in 
  the HanOS kernel. It includes standard headers and defines various
  configuration options and default values for the kernel.

@endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define ENABLE_KLOG_DEBUG       false   /* default: false */
#define ENABLE_MEM_DEBUG        false   /* default: false */
#define ENABLE_BASH             false   /* default: false */

/* Microkernel self-test: IPC endpoints and handles at boot. */
#define ENABLE_MICROKERNEL_SELFTEST false

/* Spawn the userspace input server instead of the in-kernel keyboard path. */
#define ENABLE_INPUT_SERVER     true

/* Spawn the userspace console server and forward terminal output to it. */
#define ENABLE_CONSOLE_SERVER   true

/* Memory allocator selection */
#define USE_BUDDY_ALLOCATOR     true    /* true: buddy, false: bitmap */

#if !ENABLE_BASH
#define DEFAULT_SHELL_APP       "/bin/init"
#else
#define DEFAULT_SHELL_APP       "/usr/bin/bash"
#endif

#define DEFAULT_INPUT_SVR       "/bin/input"
#define DEFAULT_CONSOLE_SVR     "/bin/console"

#define DEFAULT_TZ_SEC_SHIFT    (8 * 60 * 60)

typedef struct {
    uint64_t screen_hor_size;
    uint64_t screen_ver_size;
    uint64_t prefer_res_x;
    uint64_t prefer_res_y;
    uint64_t actual_res_x;
    uint64_t actual_res_y;
} computer_info_t;
