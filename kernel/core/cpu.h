/**-----------------------------------------------------------------------------
 @file    cpu.h
 @brief   Definition of CPU related data structures and macros
 @details
 @verbatim
  e.g., Read & write control registers, model specific registers and port
  input & output.
 @endverbatim
 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define MSR_PAT         0x0277

#define MSR_FS_BASE     0xC0000100
#define MSR_GS_BASE     0xC0000101
#define MSR_KGS_BASE    0xC0000102

#define MSR_EFER        0xC0000080

/* @see https://wiki.osdev.org/SYSCALL#AMD:_SYSCALL.2FSYSRET */
/* Ring 0 and Ring 3 Segment bases, as well as SYSCALL EIP.
 * Low 32 bits = SYSCALL EIP, bits 32-47 are kernel segment base, bits 48-63 are
 * user segment base.
 */
#define MSR_STAR        0xC0000081
/* The kernel's RIP SYSCALL entry for 64 bit software */
#define MSR_LSTAR       0xC0000082
/* The kernel's RIP for SYSCALL in compatibility mode */
#define MSR_CSTAR       0xC0000083
/* The low 32 bits are the SYSCALL flag mask. If a bit in this is set, the
 * corresponding bit in rFLAGS is cleared
 */
#define MSR_SFMASK      0xC0000084

/* NOTE: these instructions assume a flat segmented memory model (paging
 * allowed). They require that "the code-segment base, limit, and attributes
 * (except for CPL) are consistent for all application and system processes."
 * --AMD System programming
 *
 * SYSCALL loads CS from STAR 47:32. It masks EFLAGS with SFMASK. Next it stores
 * EIP in ECX. It then loads EIP from STAR 32:0 and SS from STAR 47:32 + 8. It
 * then executes.
 *
 * Note that the Kernel does not automatically have a kernel stack loaded. This
 * is the handler's responsibility.
 *
 * SYSRET loads CS from STAR 63:48. It loads EIP from ECX and SS from STAR 63:48
 * + 8.
 *
 * Note that the User stack is not automatically loaded. Also note that ECX must
 * be preserved
 *
 * - 64 bit mode -
 *
 * The operation in 64 bit mode is the same, except that RIP is loaded from
 * LSTAR, or CSTAR of in IA32-e submode (A.K.A. compatibility mode). It also
 * respectively saves and loads RFLAGS to and from R11. As well, in Long Mode,
 * userland CS will be loaded from STAR 63:48 + 16 and userland SS from STAR
 * 63:48 + 8 on SYSRET. Therefore, you might need to setup your GDT accordingly.
 *
 */

/* Read & write control registers on x64 CPUs, which contains flags controlling
 * CPU features related to memory protection, multitasking, paging, etc.
 * 6 CRs available: cr0, cr1, cr2, cr3, cr4 and cr8
 */
#define read_cr(cr, n)  asm volatile("mov %%" cr ", %%rax;" \
                                     "mov %%rax, %0"        \
                                     : "=g"(*(n))           \
                                     :                      \
                                     : "rax");

#define write_cr(cr, n) asm volatile("mov %0, %%rax;"       \
                                    "mov %%rax, %%" cr      \
                                    :                       \
                                    : "g"(n)                \
                                    : "rax");

/* Read & write model specific registers on x64 CPUs, which contains flags controlling
 * OS-relevant things such as memory type-range, sysenter/sysexit, local APIC, etc.
 * Ref: https://wiki.osdev.org/Model_Specific_Registers
 */
static inline uint64_t read_msr(uint32_t msr)
{   
    uint32_t low, high;
    
    asm volatile("mov %[msr], %%ecx;"
                 "rdmsr;"
                 "mov %%eax, %[low];"
                 "mov %%edx, %[high];" 
                 : [low] "=g"(low), [high] "=g"(high)
                 : [msr] "g"(msr)
                 : "eax", "ecx", "edx");
    
    uint64_t val = ((uint64_t)high << 32) | low;
    return val;
}

static inline void write_msr(uint32_t msr, uint64_t val)
{
    uint32_t low = val & UINT32_MAX;
    uint32_t high = (val >> 32) & UINT32_MAX;

    asm volatile("mov %[msr], %%ecx;"
                 "mov %[low], %%eax;"
                 "mov %[high], %%edx;"
                 "wrmsr;"
                 :   
                 : [msr] "g"(msr), [low] "g"(low), [high] "g"(high)
                 : "eax", "ecx", "edx");
}

/* Port I/O functions */
static inline uint8_t port_inb(uint16_t port)
{
    uint8_t ret;
    asm volatile("inb %%dx, %%al" : "=a"(ret) : "d"(port));
    return ret;
}

static inline uint16_t port_inw(uint16_t port)
{
    uint16_t ret;
    asm volatile("inw %%dx, %%ax" : "=a"(ret) : "d"(port));
    return ret;
}

static inline uint32_t port_ind(uint16_t port)
{
    uint32_t ret;
    asm volatile("inl %%dx, %%eax" : "=a"(ret) : "d"(port));
    return ret;
}

static inline void port_outb(uint16_t port, uint8_t data)
{
    asm volatile("outb %%al, %%dx" : : "a"(data), "d"(port));
}

static inline void port_outw(uint16_t port, uint16_t data)
{
    asm volatile("outw %%ax, %%dx" : : "a"(data), "d"(port));
}

static inline void port_outd(uint16_t port, uint32_t data)
{
    asm volatile("outl %%eax, %%dx" : : "a"(data), "d"(port));
}

static inline void port_insw(uint16_t port, void* addr, uint32_t count)
{
    asm volatile("rep insw" : "+D" (addr), "+c" (count) : "d" (port) : "memory");
}

static inline void port_outsw(uint16_t port, const void* addr, uint32_t count)
{
    asm volatile("rep outsw" : "+S" (addr), "+c" (count) : "d" (port));
}

static inline void port_io_wait()
{
    for (int i = 0; i < 4; i++) {
        asm volatile("nop");
    }
}

/* memory mapped I/O functions */
static inline void mmio_outb(void *p, uint8_t data)
{
    *(volatile uint8_t *)(p) = data;
}

static inline uint8_t mmio_inb(void *p)
{
    return *(volatile uint8_t *)(p);
}

static inline void mmio_outw(void *p, uint16_t data)
{
    *(volatile uint16_t *)(p) = data;
}

static inline uint16_t mmio_inw(void *p)
{
    return *(volatile uint16_t *)(p);
}

static inline void mmio_outd(void *p, uint32_t data)
{
    *(volatile uint32_t *)(p) = data;
}

static inline uint32_t mmio_ind(void *p)
{
    return *(volatile uint32_t *)(p);
}

static inline void mmio_outl(void *p, uint64_t data)
{
    *(volatile uint64_t *)(p) = data;
}

static inline uint64_t mmio_inl(void *p)
{
    return *(volatile uint64_t *)(p);
}

static inline void mmio_inn(
    void *dst, const volatile void *src, size_t bytes)
{
    volatile uint8_t *s = (volatile uint8_t *)src;
    uint8_t *d = (uint8_t *)dst;
    while (bytes > 0) {
        *d =  *s;
        ++s;
        ++d;
        --bytes;
    }
}

/* CPU related functions and data structures*/
void cpu_init();
bool cpu_ok();

typedef struct {
    uint32_t func;
    uint32_t param;
    enum {
        CPUID_REG_EAX,
        CPUID_REG_EBX,
        CPUID_REG_ECX,
        CPUID_REG_EDX
    } reg;
    uint32_t mask;
} cpuid_feature_t;

static const cpuid_feature_t CPUID_FEATURE_PAT  = {
    .func = 0x00000001,
    .reg = CPUID_REG_EDX,
    .mask = 1 << 16 };

static const cpuid_feature_t CPUID_FEATURE_APIC = {
    .func = 0x00000001,
    .reg = CPUID_REG_EDX,
    .mask = 1 << 9 };

bool cpuid_check_feature(cpuid_feature_t feature);

