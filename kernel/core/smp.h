///-----------------------------------------------------------------------------
///
/// @file    smp.h
/// @brief   Definition of SMP related data structures
/// @details
///
///   Symmetric Multiprocessing (or SMP) is one method of having multiple
///   processors in one computer system.
///
/// @author  JW
/// @date    Jan 2, 2022
///
///-----------------------------------------------------------------------------
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
    uint32_t unused0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t unused1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t unused2;
    uint32_t iopb_offset;
} tss_t;

typedef struct {
    uint16_t cpu_id;
    uint16_t lapic_id;
    bool is_bsp;
    tss_t tss;
} cpu_t;

typedef struct {
    uint16_t num_cpus;
    cpu_t cpus[CPU_MAX];
} smp_info_t;

void smp_init();
const smp_info_t* smp_get_info();
cpu_t* smp_get_current_info();
