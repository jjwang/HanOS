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
#define ENABLE_MICROKERNEL_SELFTEST true

/* The input, console, block and file servers (block, VFS, pipe, tty, FAT32)
 * always run in userspace; there is no in-kernel fallback any more. */

/* Memory allocator selection */
#define USE_BUDDY_ALLOCATOR     true    /* true: buddy, false: bitmap */

#if !ENABLE_BASH
#define DEFAULT_SHELL_APP       "/bin/init"
#else
#define DEFAULT_SHELL_APP       "/usr/bin/bash"
#endif

#define DEFAULT_INPUT_SVR       "/bin/input"
#define DEFAULT_CONSOLE_SVR     "/bin/console"
#define DEFAULT_BLOCK_SVR       "/bin/block"
#define DEFAULT_VFS_SVR         "/bin/vfs"
#define DEFAULT_PIPE_SVR        "/bin/pipe"
#define DEFAULT_TTY_SVR         "/bin/tty"
#define DEFAULT_FAT32_SVR       "/bin/fat32"

#define DEFAULT_TZ_SEC_SHIFT    (8 * 60 * 60)

typedef struct {
    uint64_t screen_hor_size;
    uint64_t screen_ver_size;
    uint64_t prefer_res_x;
    uint64_t prefer_res_y;
    uint64_t actual_res_x;
    uint64_t actual_res_y;
} computer_info_t;
