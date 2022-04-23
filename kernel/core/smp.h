/**-----------------------------------------------------------------------------

 @file    smp.h
 @brief   Definition of SMP related data structures
 @details
 @verbatim

  Symmetric Multiprocessing (or SMP) is one method of having multiple
  processors in one computer system.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SMP_TRAMPOLINE_BLOB_ADDR        0x70000
#define SMP_AP_BOOT_COUNTER_ADDR        0xff0

#define SMP_TRAMPOLINE_ARG_IDTPTR       0xfa0
#define SMP_TRAMPOLINE_ARG_RSP          0xfb0
#define SMP_TRAMPOLINE_ARG_ENTRYPOINT   0xfc0
#define SMP_TRAMPOLINE_ARG_CR3          0xfd0
#define SMP_TRAMPOLINE_ARG_CPUINFO      0xfe0

#define CPU_MAX                         256

typedef struct [[gnu::packed]] {
    uint32_t reserved;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;

    uint32_t reserved_1;
    uint32_t reserved_2;

    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;

    uint64_t reserved_3;
    uint16_t reserved_4;
    uint16_t io_bitmap_offset;
} tss_t;

typedef struct {
    tss_t tss;
    uint16_t cpu_id;
    uint16_t lapic_id;
    bool is_bsp;
    uint8_t reserved[3];
} cpu_t;

typedef struct {
    cpu_t cpus[CPU_MAX];
    uint16_t num_cpus;
} smp_info_t;

void smp_init(void);
const smp_info_t* smp_get_info(void);
cpu_t* smp_get_current_cpu(bool force_read);

