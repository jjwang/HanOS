/**-----------------------------------------------------------------------------

 @file    mtrr.h
 @brief   Definition of MTRR macros and save/restore functions
 @details
 @verbatim
 
  Save and modify MTRR settings when booting system, and restore every core by
  modified values.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <sys/cpu.h>

#define MTRR_CACHE_UNCACHEABLE          0
#define MTRR_CACHE_WRITE_COMBINING      1
#define MTRR_CACHE_WRITE_THROUGH        4
#define MTRR_CACHE_WRITE_PROTECTED      5
#define MTRR_CACHE_WRITE_BACK           6

void mtrr_save(uint16_t cpu_id, void *framebuffer);
void mtrr_restore(uint16_t cpu_id);

