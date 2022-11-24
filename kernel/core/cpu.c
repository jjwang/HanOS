/**-----------------------------------------------------------------------------
 @file    cpu.c
 @brief   Implementation of CPU related functions
 @details
 @verbatim
   e.g., CPU initialization...
 @endverbatim
 **-----------------------------------------------------------------------------
 */
#include <core/cpu.h>
#include <lib/klog.h>
#include <lib/memutils.h>
#include <lib/string.h>

static bool cpu_initialized = false;

static uint32_t cpu_model = 0;
static uint32_t cpu_family = 0;
static char cpu_model_name[60] = {0}; /* Should no less than 48 */
static char cpu_manufacturer[60] = {0};

void cpuid(uint32_t func, uint32_t param, uint32_t* eax, uint32_t* ebx, uint32_t* ecx, uint32_t* edx)
{   
    asm volatile("mov %[func], %%eax;"
                 "mov %[param], %%ecx;"
                 "cpuid;"
                 "mov %%eax, %[ieax];"
                 "mov %%ebx, %[iebx];"
                 "mov %%ecx, %[iecx];"
                 "mov %%edx, %[iedx];"
                 : [ieax] "=g"(*eax), [iebx] "=g"(*ebx), [iecx] "=g"(*ecx), [iedx] "=g"(*edx)
                 : [func] "g"(func), [param] "g"(param)
                 : "%eax", "%ebx", "%ecx", "%edx", "memory");
}

bool cpuid_check_feature(cpuid_feature_t feature)
{   
    uint32_t regs[4]; 
    uint64_t maxleaf, maxhighleaf;
    
    /* get highest supported leaf */
    cpuid(0, 0, &regs[CPUID_REG_EAX], &regs[CPUID_REG_EBX],
        &regs[CPUID_REG_ECX], &regs[CPUID_REG_EDX]);
    maxleaf = regs[CPUID_REG_EAX];
    
    /* get highest supported extended leaf */
    cpuid(0x80000000, 0, &regs[CPUID_REG_EAX], &regs[CPUID_REG_EBX],
        &regs[CPUID_REG_ECX], &regs[CPUID_REG_EDX]);
    maxhighleaf = regs[CPUID_REG_EAX];

    /* is the leaf being requested supported? */
    if ((maxleaf < feature.func && feature.func < 0x80000000)
        || (maxhighleaf < feature.func && feature.func >= 0x80000000)) {
        klogi("CPUID leaf %x not supported\n", feature.func);
        return false;
    }

    /* is the feature supported? */
    cpuid(feature.func, feature.param, &regs[CPUID_REG_EAX], &regs[CPUID_REG_EBX],
        &regs[CPUID_REG_ECX], &regs[CPUID_REG_EDX]);
    if (regs[feature.reg] & feature.mask)
        return true;

    return false;
}

void cpu_init()
{
    uint64_t patval, vcr0, vcr4;

    /* Page Attribute Table (PAT) allows for setting the memory attribute at
     * the page level granularity. PAT is complementary to the MTRR settings
     * which allows for setting of memory types over physical address ranges.
     * However, PAT is more flexible than MTRR due to its capability to set
     * attributes at page level and also due to the fact that there are no
     * hardware limitations on number of such attribute settings allowed.
     * Added flexibility comes with guidelines for not having memory type
     * aliasing for the same physical memory with multiple virtual addresses.
     *
     * if PAT is supported, set pa4 in the PAT to write-combining
     */
    if (cpuid_check_feature(CPUID_FEATURE_PAT)) {
        patval = read_msr(MSR_PAT);
        patval &= ~(0b111ULL << 32);
        patval |= 0b001ULL << 32; 
        write_msr(MSR_PAT, patval);
    }  

    /* The EM and MP flags of CR0 control how the processor reacts to
     * coprocessor instructions.
     *
     * The EM bit indicates whether coprocessor functions are to be emulated.
     * If the processor finds EM set when executing an ESC instruction, it
     * signals exception 7, giving the exception handler an opportunity to
     * emulate the ESC instruction.
     *
     * The MP (monitor coprocessor) bit indicates whether a coprocessor is
     * actually attached. The MP flag controls the function of the WAIT
     * instruction. If, when executing a WAIT instruction, the CPU finds MP
     * set, then it tests the TS flag; it does not otherwise test TS during
     * a WAIT instruction. If it finds TS set under these conditions, the CPU
     * signals exception 7.
     *
     * clear the CR0.EM bit and set the CR0.MP bit
     */
    read_cr("cr0", &vcr0);
    vcr0 &= ~(1 << 2); 
    vcr0 |= 1 << 1;
    write_cr("cr0", vcr0);

    /* OSFXSR: Enables 128-bit SSE support.
     * OSXMMEXCPT: Enables the #XF exception.
     *
     * set the CR4.OSFXSR and CR4.OSXMMEXCPT bit
     */
    read_cr("cr4", &vcr4);
    vcr4 |= 1 << 9;
    vcr4 |= 1 << 10; 
    write_cr("cr4", vcr4);

    uint32_t x, y, na;
    cpuid(0, 0, &na, &y, &na, &na);

    strcpy(cpu_manufacturer, "Unknown");

    if (y == 0x756e6547) {
        cpuid(1, 0, &x, &y, &na, &na);
        strcpy(cpu_manufacturer, "Intel");
        cpu_model = (x >> 4) & 0x0F;
        cpu_family = (x >> 8) & 0x0F;
    } else if (y == 0x68747541) {
        cpuid(1, 0, &x, &na, &na, &na);
        strcpy(cpu_manufacturer, "AMD");
        cpu_model = (x >> 4) & 0x0F;
        cpu_family = (x >> 8) & 0x0F;
    }

    klogi("CPU: model 0x%2x, family 0x%2x, manufacturer %s\n",
          cpu_model, cpu_family, cpu_manufacturer);

    cpuid(0x80000000, 0, &x, &na, &na, &na);
    if (x >= 0x80000004) {
        uint32_t brand[12];
        cpuid(0x80000002, 0, &(brand[0]), &(brand[1]), &(brand[2]), &(brand[3]));
        cpuid(0x80000003, 0, &(brand[4]), &(brand[5]), &(brand[6]), &(brand[7]));
        cpuid(0x80000004, 0, &(brand[8]), &(brand[9]), &(brand[10]), &(brand[11]));
        memcpy(cpu_model_name, brand, 48);
        cpu_model_name[48] = '\0';
        klogi("CPU: %s\n", cpu_model_name);
    }

    cpu_initialized = true;
}

bool cpu_ok()
{
    return cpu_initialized;
}

char *cpu_get_model_name()
{
    return cpu_model_name;
}
