#pragma once

#include <sys/cpu.h>

/* MTRR, or Memory-Type Range Registers are a group of x86 Model Specific
 * Registers providing a way to control access and cacheability of physical
 * memory regions.
 */
#define MTRR_CACHE_UNCACHEABLE          0
#define MTRR_CACHE_WRITE_COMBINING      1
#define MTRR_CACHE_WRITE_THROUGH        4
#define MTRR_CACHE_WRITE_PROTECTED      5
#define MTRR_CACHE_WRITE_BACK           6

void mtrr_save(uint16_t cpu_id);
void mtrr_set(uint16_t cpu_id);
void mtrr_restore(uint16_t cpu_id);

