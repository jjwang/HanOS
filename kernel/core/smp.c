///-----------------------------------------------------------------------------
///
/// @file    smp.c
/// @brief   Implementation of SMP related functions
/// @details
///
///   Symmetric Multiprocessing (or SMP) is one method of having multiple
///   processors in one computer system.
///
///   Ref: https://wiki.osdev.org/SMP
///
/// @author  JW
/// @date    Jan 2, 2022
///
///-----------------------------------------------------------------------------
#include <stddef.h>
#include <lib/klog.h>
#include <lib/kmalloc.h>
#include <lib/memutils.h>
#include <lib/time.h>
#include <core/mm.h>
#include <core/cpu.h>
#include <core/smp.h>
#include <core/gdt.h>
#include <core/hpet.h>
#include <core/madt.h>
#include <core/apic.h>

extern uint8_t smp_trampoline_blob_start, smp_trampoline_blob_end;

static volatile int* ap_boot_counter = (volatile int*)PHYS_TO_VIRT(SMP_AP_BOOT_COUNTER_ADDR);

static smp_info_t info;

const smp_info_t* smp_get_info()
{
    return &info;
}

cpu_t* smp_get_current_info()
{
    return (cpu_t*)read_msr(MSR_GS_BASE);
}

static void init_tss(cpu_t* cpuinfo)
{
    cpuinfo->tss.iopb_offset = sizeof(tss_t);
    cpuinfo->tss.rsp0 = 0; // will be filled in by the scheduler
    gdt_install_tss(&(cpuinfo->tss));
}

// AP's will run this code upon boot
_Noreturn void smp_ap_entrypoint(cpu_t* cpuinfo)
{
    klog_printf("SMP: continue to initialize core %04x\n", cpuinfo->cpu_id);

    // initialize cpu features
    gdt_init();
    cpu_init();

    // initialze gdt and make a tss
    init_tss(cpuinfo);
    
    // put cpu information in gs
    write_msr(MSR_GS_BASE, (uint64_t)cpuinfo);

    // enable the apic
    apic_enable();

    // wait for scheduler
    asm volatile("sti");
    while (true)
        asm volatile("hlt");
}

// Trampoline code is used by BSP to boot other secondary CPUs. At startup,
// BSP wakeup secondary CPUs by sending a APIC INIT command with address
// where the secondary CPUs should start to run.
//
static void prepare_trampoline()
{
    // copy the trampoline blob to 0x1000 physical
    uint64_t trmpblobsize = (uint64_t)&smp_trampoline_blob_end - (uint64_t)&smp_trampoline_blob_start;

    memcpy((void*)PHYS_TO_VIRT(SMP_TRAMPOLINE_BLOB_ADDR), &smp_trampoline_blob_start, trmpblobsize);

    // pass arguments to trampoline code
    read_cr("cr3", (uint64_t*)PHYS_TO_VIRT(SMP_TRAMPOLINE_ARG_CR3));
    asm volatile("sidt %0"
                 : "=m"(*(uint64_t*)PHYS_TO_VIRT(SMP_TRAMPOLINE_ARG_IDTPTR))
                 :
                 :);
    *((uint64_t*)PHYS_TO_VIRT(SMP_TRAMPOLINE_ARG_ENTRYPOINT)) = (uint64_t)&smp_ap_entrypoint;
}

void smp_init()
{
    // identity map first mb for the trampoline
    vmm_map(0, 0, NUM_PAGES(0x100000), VMM_FLAGS_DEFAULT);

    prepare_trampoline();

    // get lapic info from the madt
    uint64_t cpunum = madt_get_num_lapic();
    madt_record_lapic_t** lapics = madt_get_lapics();
    klog_printf("SMP: core number is %d\n", cpunum);
    
    // loop through the lapic's present and initialize them one by one
    for (uint64_t i = 0; i < cpunum; i++) {
        int counter_prev = *ap_boot_counter, counter_curr;

        // if cpu is not online capable, do not initialize it
        if (!(lapics[i]->flags & MADT_LAPIC_FLAG_ONLINE_CAPABLE)
            && !(lapics[i]->flags & MADT_LAPIC_FLAG_ENABLED)) {
            klog_printf("SMP: core %d is not enabled or online capable\n", lapics[i]->proc_id);
            continue;
        }

        info.cpus[info.num_cpus].lapic_id = lapics[i]->apic_id;
        info.cpus[info.num_cpus].cpu_id = info.num_cpus;

        // if cpu is the bootstrap processor, do not initialize it
        if (apic_read_reg(APIC_REG_ID) == lapics[i]->apic_id) {
            klog_printf("SMP: core %d is BSP\n", lapics[i]->proc_id);
            info.cpus[info.num_cpus].is_bsp = true;
            write_msr(MSR_GS_BASE, (uint64_t)&info.cpus[info.num_cpus]);
            init_tss(&info.cpus[info.num_cpus]);
            info.num_cpus++;
            continue;
        }

        klog_printf("SMP: initializing core %d...\n", lapics[i]->proc_id);

        // allocate and pass the stack
        void* stack = kmalloc(PAGE_SIZE);
        *((uint64_t*)PHYS_TO_VIRT(SMP_TRAMPOLINE_ARG_RSP)) = (uint64_t)stack + PAGE_SIZE;

        // pass cpu information
        *((uint64_t*)PHYS_TO_VIRT(SMP_TRAMPOLINE_ARG_CPUINFO)) = (uint64_t)&info.cpus[info.num_cpus];

        // send the init ipi
        apic_send_ipi(lapics[i]->apic_id, 0, APIC_IPI_TYPE_INIT);
        hpet_nanosleep(MILLIS_TO_NANOS(10));

        bool success = false;
        for (int k = 0; k < 2; k++) { // send startup ipi 2 times
            apic_send_ipi(lapics[i]->apic_id, SMP_TRAMPOLINE_BLOB_ADDR / PAGE_SIZE, APIC_IPI_TYPE_STARTUP);
            // check if cpu has started
            for (int j = 0; j < 20; j++) {
                counter_curr = *ap_boot_counter;
                if (counter_curr != counter_prev) {
                    success = true;
                    break;
                }
                hpet_nanosleep(MILLIS_TO_NANOS(10));
            }
            if (success)
                break;
        }

        if (!success) {
            klog_printf("SMP: core %d initialization failed\n", lapics[i]->proc_id);
            kmfree(stack);
        } else {
            klog_printf("SMP: core %d initialization successed\n", lapics[i]->proc_id);
            info.cpus[info.num_cpus].is_bsp = false;
            info.num_cpus++;
        }
    }

    klog_printf("SMP: %d processors brought up.\n", info.num_cpus);

    // identity mapping is no longer needed
    vmm_unmap(0, NUM_PAGES(0x100000));
}
