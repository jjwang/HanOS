/**-----------------------------------------------------------------------------

 @file    smp.c
 @brief   SMP initialization and other support functions
 @details
 @verbatim

  Symmetric Multiprocessing (or SMP) is one method of having multiple
  processors in one computer system.

  - Nov 2022: Need to check why AP cannot be launched when enabling SSE support.
              (Root Cause: ???)
  - Apr 2024: Finally we get SMP working very well by the following works:
              1) Enable interrupt after all AP cores are initialized.
                 At beginning, we find APIC timer does not work for AP core.
                 This is caused by abnormal exit from timer ISR function.
              2) Init syscall for AP core to avoid invalid opcode exception.
                 For Intel 64bit, IA32_EFER.SCE must be set, or SYSCALL will
                 result in a #UD exception. IA32_EFER is an MSR at 0xC0000080,
                 and SCE (SYSCALL Enable) is its 0th bit.

 @endverbatim
  Ref: https://wiki.osdev.org/SMP

 **-----------------------------------------------------------------------------
 */
#include <stddef.h>
#include <kconfig.h>

#include <libc/string.h>

#include <base/klog.h>
#include <base/kmalloc.h>
#include <base/time.h>
#include <mm/mm.h>
#include <sys/idt.h>
#include <sys/cpu.h>
#include <sys/smp.h>
#include <sys/gdt.h>
#include <sys/hpet.h>
#include <sys/madt.h>
#include <sys/apic.h>
#include <sys/pit.h>
#include <sys/panic.h>
#include <proc/syscall.h>
#include <proc/sched.h>

extern uint8_t smp_trampoline_blob_start, smp_trampoline_blob_end;

uint8_t smp_halt_ipi_vector = 0;

static volatile int *ap_boot_counter =
    (volatile int *) PHYS_TO_VIRT(SMP_AP_BOOT_COUNTER_ADDR);

static smp_info_t *smp_info = NULL;

static lock_t smp_lock = lock_new();

bool smp_is_initialized(void)
{
    if (smp_info == NULL) return false;
    return smp_info->initialized;
}

smp_info_t *smp_get_info()
{
    if (!smp_is_initialized()) {
        return NULL;
    } else {
        return smp_info;
    }
}

/* The reason why there is a force_read parameter here is that when
 * initializing SMP, it should not return NULL if we know this CPU
 * is already initialized. Check the code in proc/sched.c whose
 * parameter is true.
 */
cpu_t *smp_get_current_cpu(bool force_read)
{
    if (smp_is_initialized() || force_read) {
        cpu_t *cpu = (cpu_t *) read_msr(MSR_KERN_GS_BASE);
        if (cpu == NULL)
            cpu = (cpu_t *) read_msr(MSR_GS_BASE);
        return cpu;
    } else {
        return NULL;
    }
}

/* This is the only function in this module which will be called very
 * oftenly in syscall functions.
 */
bool cpu_set_errno(int64_t val)
{
    lock_lock(&smp_lock);
    if (smp_is_initialized()) {
        cpu_t *cpu = (cpu_t *) read_msr(MSR_KERN_GS_BASE);
        if (cpu == NULL)
            cpu = (cpu_t *) read_msr(MSR_GS_BASE);
        if (cpu != NULL) {
            cpu->errno = val;
            lock_release(&smp_lock);
            return true;
        }
    }
    lock_release(&smp_lock);
    return false;
}

void cpu_debug(void)
{
    if (smp_is_initialized()) {
        cpu_t *cpu = (cpu_t *) read_msr(MSR_KERN_GS_BASE);
        if (cpu == NULL)
            cpu = (cpu_t *) read_msr(MSR_GS_BASE);
        if (cpu != NULL) {
            klogd("CPU: total_num %ld, current id %ld, kernel stack 0x%016lx\n",
                  smp_info->num_cpus, cpu->cpu_id, cpu->tss.rsp0);
            return;
        }
    }
    klogd("CPU: uninitialized\n");
}

uint16_t smp_get_current_cpu_id(void)
{
    uint32_t cpuid_ebx;  /* EBX register output from CPUID */

    /* Execute CPUID instruction to get processor information:
     * - EAX = 1 (function code for processor info)
     * - EBX[31:24] = APIC/CPU ID (unique per CPU in SMP system)
     */
    asm volatile (
        "movl $1, %%eax\n"    /* Set CPUID function code to 1 */
        "cpuid\n"             /* Execute CPUID instruction */
        "movl %%ebx, %[ebx]\n"/* Save EBX value to cpuid_ebx variable */
        : [ebx] "=r"(cpuid_ebx)
        :
        : "eax", "ebx", "ecx", "edx"  /* Clobbered registers */
    );  

    /* Extract 8-bit CPU ID from EBX (shift right 24 bits + mask) */
    return (cpuid_ebx >> 24) & 0xFF;
}

void init_tss(cpu_t * cpuinfo)
{
    gdt_install_tss(cpuinfo);
}

/* AP's will run this code upon boot */
_Noreturn void smp_ap_entrypoint(cpu_t * cpuinfo)
{
    /* initialize cpu features */
    cpu_init(cpuinfo->cpu_id);
    gdt_init(cpuinfo);

    klogi("SMP: continue to initialize core %ld (0x%016lx)\n",
          cpuinfo->cpu_id, cpuinfo);

    /* put cpu information in gs */
    write_msr(MSR_GS_BASE, (uint64_t) cpuinfo);
    write_msr(MSR_KERN_GS_BASE, (uint64_t) cpuinfo);

    uint64_t msr_gs_base = read_msr(MSR_GS_BASE);
    uint64_t msr_kern_gs_base = read_msr(MSR_KERN_GS_BASE);
    klogi("SMP: core %ld MSR_GS_BASE 0x%016lx MSR_KERN_GS_BASE 0x%016lx\n",
          cpuinfo->cpu_id, msr_gs_base, msr_kern_gs_base);

    /* initialze gdt and make a tss */
    for (uint64_t dl = 0; dl < 100; dl++)
        asm volatile ("nop;");
    init_tss(cpuinfo);

    /* enable the apic */
    apic_enable();

    /* enable syscall. this should be called for each CPU */
    syscall_init();

    /* initialize and wait for scheduler */
    sched_init("idle", cpuinfo->cpu_id);

    /* Wait for finishing the initialization of all CPU cores */
    while (!smp_is_initialized()) {
        asm volatile("mfence" : : : "memory");
    }

    /* Start the timer for context switch */
    apic_timer_start();

    /* Remember we need to init all CPUs and then make hearts beat */
    asm volatile ("sti");

    klogi("SMP: finish initialization of core %ld (0x%016lx)\n",
          cpuinfo->cpu_id, cpuinfo);

    uint64_t cr0;
    read_cr("cr0", &cr0);

    bool cache_disabled = cr0 & (1ULL << 30);  /* CD = bit 30 */
    bool not_write_through = cr0 & (1ULL << 29); /* NW = bit 29 */

    if (cache_disabled) {
        klogw("Warning: CPU cache DISABLED (CR0.CD=1)\n");
    }   
    if (not_write_through) {
        klogw("Warning: CPU write-through DISABLED (CR0.NW=1)\n");
    }

    while (true)
        asm volatile ("hlt");
}

/* Trampoline code is used by BSP to boot other secondary CPUs. At startup,
 * BSP wakeup secondary CPUs by sending a APIC INIT command with address
 * where the secondary CPUs should start to run.
 */
static void prepare_trampoline()
{
    /* copy the trampoline blob to 0x1000 physical */
    uint64_t trmpblobsize =
        (uint64_t) & smp_trampoline_blob_end -
        (uint64_t) & smp_trampoline_blob_start;

    memcpy((void *) PHYS_TO_VIRT(SMP_TRAMPOLINE_BLOB_ADDR),
           &smp_trampoline_blob_start, trmpblobsize);

    /* pass arguments to trampoline code */
    read_cr("cr3", (uint64_t *) PHYS_TO_VIRT(SMP_TRAMPOLINE_ARG_CR3));
    asm volatile ("sidt %0":"=m"
                  (*(uint64_t *) PHYS_TO_VIRT(SMP_TRAMPOLINE_ARG_IDTPTR))
                  ::);
    *((uint64_t *) PHYS_TO_VIRT(SMP_TRAMPOLINE_ARG_ENTRYPOINT)) =
        (uint64_t) & smp_ap_entrypoint;

    klogi("Trampoline start 0x%016lx end 0x%016lx\n",
          (uint64_t) & smp_trampoline_blob_start,
          (uint64_t) & smp_trampoline_blob_end);
}

void smp_init()
{
    smp_info = (smp_info_t *) kmalloc(sizeof(smp_info_t));
    memset(smp_info, 0, sizeof(smp_info_t));

    smp_info->initialized = false;

    /* identity map first mb for the trampoline */
    vmm_map(NULL, 0, 0, NUM_PAGES(0x100000), VMM_FLAGS_DEFAULT);

    prepare_trampoline();

    /* get lapic info from the madt */
    uint64_t cpunum = madt_get_num_lapic();
    madt_record_lapic_t **lapics = madt_get_lapics();

    klogi("SMP: core number is %ld\n", cpunum);

    /* We must have a BSP core whose id is zero */
    memset(&(smp_info->cpus[0]), 0, sizeof(cpu_t));
    smp_info->cpus[0].is_bsp = false;

    for (uint64_t i = 0; i < cpunum; i++) {
        if (apic_read_reg(APIC_REG_ID) == lapics[i]->apic_id) {
            smp_info->cpus[0].cpu_id = lapics[i]->apic_id;
            smp_info->cpus[0].lapic_id = lapics[i]->apic_id;
            smp_info->cpus[0].proc_id = lapics[i]->proc_id;
            smp_info->cpus[0].is_bsp = true;

            klogi("SMP: core 0 with proc id %ld and apic id 0x%016lx is BSP\n",
                  lapics[i]->proc_id, lapics[i]->apic_id);
            break;
        }
    }

    if (!smp_info->cpus[0].is_bsp) {
        kpanic("SMP: Cannot find BSP core.\n");
    }

    write_msr(MSR_GS_BASE, (uint64_t) & (smp_info->cpus[0]));
    write_msr(MSR_KERN_GS_BASE, (uint64_t) & (smp_info->cpus[0]));

    uint64_t msr_gs_base = read_msr(MSR_GS_BASE);
    uint64_t msr_kern_gs_base = read_msr(MSR_KERN_GS_BASE);
    klogi("SMP: core %ld MSR_GS_BASE 0x%016lx MSR_KERN_GS_BASE 0x%016lx\n",
          0, msr_gs_base, msr_kern_gs_base);

    init_tss(&(smp_info->cpus[0]));

    smp_info->num_cpus = 1;

    /* loop through the lapic's present and initialize them one by one */
    for (uint64_t i = 0; i < cpunum; i++) {
        uint64_t coreid = 0;
        if (apic_read_reg(APIC_REG_ID) != lapics[i]->apic_id) {
            coreid = smp_info->num_cpus;
        } else {
            /* It is a bootstrap processor */
            continue;
        }

        memset(&(smp_info->cpus[coreid]), 0, sizeof(cpu_t));
        int counter_prev = *ap_boot_counter;

        /* if cpu is not online capable, do not initialize it */
        if (!(lapics[i]->flags & MADT_LAPIC_FLAG_ONLINE_CAPABLE)
            && !(lapics[i]->flags & MADT_LAPIC_FLAG_ENABLED)) {
            klogi("SMP: core %ld with prod id %ld is not enabled or online "
                  "capable\n", coreid, lapics[i]->proc_id);
            continue;
        }

        smp_info->cpus[coreid].cpu_id = lapics[i]->apic_id;
        smp_info->cpus[coreid].lapic_id = lapics[i]->apic_id;
        smp_info->cpus[coreid].proc_id = lapics[i]->proc_id;

        klogi("SMP: initializing core %ld (prev: %ld) with APIC id 0x%016lx...\n",
              coreid, counter_prev, lapics[i]->apic_id);

        /* allocate and pass the stack */
        void *stack = kmalloc(STACK_SIZE);
        *((uint64_t *) PHYS_TO_VIRT(SMP_TRAMPOLINE_ARG_RSP)) =
            (uint64_t) stack + STACK_SIZE;

        /* pass cpu information */
        *((uint64_t *) PHYS_TO_VIRT(SMP_TRAMPOLINE_ARG_CPUINFO)) =
            (uint64_t) & (smp_info->cpus[coreid]);

        /* send the init ipi */
        apic_send_ipi(lapics[i]->apic_id, 0, APIC_IPI_TYPE_INIT);
        hpet_sleep(100);

        bool success = false;
        for (uint64_t k = 0; k < 2; k++) {      /* send startup ipi 2 times */
            apic_send_ipi(lapics[i]->apic_id,
                          SMP_TRAMPOLINE_BLOB_ADDR / PAGE_SIZE,
                          APIC_IPI_TYPE_STARTUP);
            hpet_sleep(100);
            /* check if cpu has started */
            for (uint64_t j = 0; j < 1000; j++) {
                int counter_curr = *ap_boot_counter;
                if (counter_curr != counter_prev) {
                    success = true;
                    break;
                }
                hpet_sleep(1);
            }
            if (success)
                break;
        }

        if (!success) {
            klogi("SMP: core %ld initialization failed\n", coreid);
            kmfree(stack);
        } else {
            klogi("SMP: core %ld initialization successed\n", coreid);
            smp_info->cpus[smp_info->num_cpus].is_bsp = false;
        }
        smp_info->num_cpus++;
        /* Must sleep here to make sure CPU is initialized one by one */
        hpet_sleep(250);
    }

    while (true) {
        if (sched_get_cpu_num() == smp_info->num_cpus - 1)
            break;
        hpet_sleep(1);
    }

    klogi("SMP: %ld processors brought up\n", smp_info->num_cpus);

    /* identity mapping is no longer needed */
    vmm_unmap(NULL, 0, NUM_PAGES(0x100000));

    smp_info->initialized = true;
    asm volatile("mfence" : : : "memory");

    /* Make the heart beat */
    asm volatile ("sti");
}

