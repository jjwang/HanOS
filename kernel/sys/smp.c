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
#include <sys/cpu.h>
#include <sys/smp.h>
#include <sys/gdt.h>
#include <sys/hpet.h>
#include <sys/madt.h>
#include <sys/apic.h>
#include <sys/pit.h>
#include <proc/syscall.h>
#include <proc/sched.h>

extern uint8_t smp_trampoline_blob_start, smp_trampoline_blob_end;

static volatile int *ap_boot_counter =
    (volatile int *) PHYS_TO_VIRT(SMP_AP_BOOT_COUNTER_ADDR);

static smp_info_t *smp_info = NULL;

static volatile bool smp_initialized = false;

static lock_t smp_lock = { 0 };

smp_info_t *smp_get_info()
{
    if (!smp_initialized) {
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
    if (smp_initialized || force_read) {
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
    if (smp_initialized) {
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
    if (smp_initialized) {
        cpu_t *cpu = (cpu_t *) read_msr(MSR_KERN_GS_BASE);
        if (cpu == NULL)
            cpu = (cpu_t *) read_msr(MSR_GS_BASE);
        if (cpu != NULL) {
            klogd("CPU: total_num %d, current id %d, kernel stack 0x%x\n",
                  smp_info->num_cpus, cpu->cpu_id, cpu->tss.rsp0);
            return;
        }
    }
    klogd("CPU: uninitialized\n");
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

    klogi("SMP: continue to initialize core %d (0x%x)\n",
          cpuinfo->cpu_id, cpuinfo);

    /* put cpu information in gs */
    write_msr(MSR_GS_BASE, (uint64_t) cpuinfo);
    write_msr(MSR_KERN_GS_BASE, (uint64_t) cpuinfo);

    /* initialze gdt and make a tss */
    for (uint64_t dl = 0; dl < 100; dl++)
        asm volatile ("nop;");
    init_tss(cpuinfo);

    /* enable the apic */
    apic_enable();

    /* enable syscall. this should be called for each CPU */
    syscall_init();

    /* initialize and wait for scheduler */
    sched_init("init", cpuinfo->cpu_id);

    while (!smp_initialized) {
        pit_wait(1);
    }

    /* Remember we need to init all CPUs and then make hearts beat */
    asm volatile ("sti");

    klogi("SMP: finish initialization of core %d (0x%x)\n",
          cpuinfo->cpu_id, cpuinfo);

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

    klogi("Trampoline start 0x%x end 0x%x\n",
          (uint64_t) & smp_trampoline_blob_start,
          (uint64_t) & smp_trampoline_blob_end);
}

void smp_init()
{
    smp_info = (smp_info_t *) kmalloc(sizeof(smp_info_t));
    memset(smp_info, 0, sizeof(smp_info_t));

    /* identity map first mb for the trampoline */
    vmm_map(NULL, 0, 0, NUM_PAGES(0x100000), VMM_FLAGS_DEFAULT);

    prepare_trampoline();

    /* get lapic info from the madt */
    uint64_t cpunum = madt_get_num_lapic();
    madt_record_lapic_t **lapics = madt_get_lapics();

#if !BSP_CORE_ONLY
    klogi("SMP: core number is %d\n", cpunum);
#endif

    /* We must have a BSP core whose id is zero */
    memset(&(smp_info->cpus[0]), 0, sizeof(cpu_t));

    smp_info->cpus[0].cpu_id = 0;
    for (uint64_t i = 0; i < cpunum; i++) {
        if (apic_read_reg(APIC_REG_ID) == lapics[i]->apic_id) {
            smp_info->cpus[0].lapic_id = lapics[i]->apic_id;
            smp_info->cpus[0].proc_id = lapics[i]->proc_id;

            klogi("SMP: core 0 with proc id %d is BSP\n",
                  lapics[i]->proc_id);
            break;
        }
    }

    smp_info->cpus[0].is_bsp = true;
    write_msr(MSR_GS_BASE, (uint64_t) & (smp_info->cpus[0]));
    write_msr(MSR_KERN_GS_BASE, (uint64_t) & (smp_info->cpus[0]));
    init_tss(&(smp_info->cpus[0]));

    smp_info->num_cpus = 1;

    /* loop through the lapic's present and initialize them one by one */
    (void) ap_boot_counter;
#if !BSP_CORE_ONLY
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
            klogi("SMP: core %d with prod id %d is not enabled or online "
                  "capable\n", coreid, lapics[i]->proc_id);
            continue;
        }

        smp_info->cpus[coreid].cpu_id = coreid;
        smp_info->cpus[coreid].lapic_id = lapics[i]->apic_id;
        smp_info->cpus[coreid].proc_id = lapics[i]->proc_id;

        klogi("SMP: initializing core %d with APIC id 0x%x...\n",
              coreid, lapics[i]->apic_id);

        /* allocate and pass the stack */
        void *stack = kmalloc(STACK_SIZE);
        *((uint64_t *) PHYS_TO_VIRT(SMP_TRAMPOLINE_ARG_RSP)) =
            (uint64_t) stack + STACK_SIZE;

        /* pass cpu information */
        *((uint64_t *) PHYS_TO_VIRT(SMP_TRAMPOLINE_ARG_CPUINFO)) =
            (uint64_t) & (smp_info->cpus[coreid]);

        /* send the init ipi */
        apic_send_ipi(lapics[i]->apic_id, 0, APIC_IPI_TYPE_INIT);
        sched_sleep(10);

        bool success = false;
        for (uint64_t k = 0; k < 2; k++) {      /* send startup ipi 2 times */
            apic_send_ipi(lapics[i]->apic_id,
                          SMP_TRAMPOLINE_BLOB_ADDR / PAGE_SIZE,
                          APIC_IPI_TYPE_STARTUP);
            /* check if cpu has started */
            for (uint64_t j = 0; j < 20; j++) {
                int counter_curr = *ap_boot_counter;
                if (counter_curr != counter_prev) {
                    success = true;
                    break;
                }
                sched_sleep(1);
            }
            if (success)
                break;
        }

        if (!success) {
            klogi("SMP: core %d initialization failed\n", coreid);
            kmfree(stack);
        } else {
            klogi("SMP: core %d initialization successed\n", coreid);
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
#endif

    klogi("SMP: %d processors brought up\n", smp_info->num_cpus);

    /* identity mapping is no longer needed */
    vmm_unmap(NULL, 0, NUM_PAGES(0x100000));

    smp_initialized = true;

    /* Make the heart beat */
    asm volatile ("sti");
}
