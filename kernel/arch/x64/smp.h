/**-----------------------------------------------------------------------------

 @file    smp.h
 @brief   Definition of SMP related data structures and functions
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

#define SMP_TRAMPOLINE_BLOB_ADDR        0x1000
#define SMP_AP_BOOT_COUNTER_ADDR        0xff0

#define SMP_TRAMPOLINE_ARG_IDTPTR       0xfa0
#define SMP_TRAMPOLINE_ARG_RSP          0xfb0
#define SMP_TRAMPOLINE_ARG_ENTRYPOINT   0xfc0
#define SMP_TRAMPOLINE_ARG_CR3          0xfd0
#define SMP_TRAMPOLINE_ARG_CPUINFO      0xfe0

#define CPU_MAX                         16

/* A canary at the low end of each kernel stack detects overflow. The
 * scheduler checks it on every context switch. The stack is 1 MiB to fit
 * large applications such as GCC. */
#define STACK_SIZE                      (PAGE_SIZE * 256)
#define STACK_CANARY                    0x535441434b4f5646ULL

typedef struct[[gnu::packed]] {
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

/*
 * In 64bit and with smp, there is a local structure for each cpu stored in the
 * gs register (other kernels can use fs). This structure contains a temporary
 * stack for the syscall, an address to store the process stack temporarily.
 */
typedef struct[[gnu::packed]] {
    int64_t errno;
    void *syscall_frame;        /* saved syscall register frame (gs:8) */
    uint64_t syscall_user_rsp;  /* caller RSP saved at syscall entry (gs:16) */
    uint64_t syscall_kstack;    /* current kernel stack top (gs:24) */
    tss_t tss;
    uint16_t cpu_id;
    uint16_t lapic_id;
    bool is_bsp;
    uint64_t fpu_storage_size;
    void (*fpu_save)(void *);
    void (*fpu_restore)(void *);
    uint16_t proc_id;
    uint8_t reserved;
} cpu_t;

/**
 * @brief Aggregate state of all CPUs discovered during SMP initialization
 */
typedef struct {
    cpu_t cpus[CPU_MAX];
    uint16_t num_cpus;
    bool initialized;
} smp_info_t;

void smp_init(void);
smp_info_t *smp_get_info(void);
cpu_t *smp_get_current_cpu(bool force_read);
uint16_t smp_get_current_cpu_id(void);
bool smp_is_initialized(void);
bool cpu_set_errno(int64_t val);
void cpu_debug(void);
