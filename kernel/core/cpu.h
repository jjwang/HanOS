/**-----------------------------------------------------------------------------

 @file    cpu.h
 @brief   Definition of CPU related data structures and macros
 @details
 @verbatim

  e.g., Read & write control registers, model specific registers and port
  input & output.

 @endverbatim
 @author  JW
 @date    Jan 2, 2022

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define MSR_PAT         0x0277
#define MSR_GS_BASE     0xC0000102
#define MSR_EFER        0xC0000080
#define MSR_STAR        0xC0000081
#define MSR_LSTAR       0xC0000082
#define MSR_SFMASK      0xC0000084

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

static const cpuid_feature_t CPUID_FEATURE_PAT = { .func = 0x00000001, .reg = CPUID_REG_EDX, .mask = 1 << 16 };

bool cpuid_check_feature(cpuid_feature_t feature);
