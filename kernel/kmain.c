/**-----------------------------------------------------------------------------

 @file    kmain.c
 @brief   Entry function of HanOS kernel
 @details
 @verbatim

  Lots of system initializations will be processed here.

  History:
    Feb 19, 2022  Added CLI task which supports some simple commands.
    May 21, 2022  Changed boot protocol to limine with corresponding
                  modifications.
@endverbatim

 **-----------------------------------------------------------------------------
 */

#include <stddef.h>

#include <kconfig.h>
#include <3rd-party/boot/limine.h>
#include <lib/time.h>
#include <lib/klog.h>
#include <lib/string.h>
#include <lib/shell.h>
#include <core/mm.h>
#include <core/gdt.h>
#include <core/idt.h>
#include <core/isr_base.h>
#include <core/smp.h>
#include <core/cmos.h>
#include <core/serial.h>
#include <core/acpi.h>
#include <core/apic.h>
#include <core/hpet.h>
#include <core/panic.h>
#include <core/pci.h>
#include <core/pit.h>
#include <device/display/term.h>
#include <device/display/edid.h>
#include <device/display/gfx.h>
#include <device/keyboard/keyboard.h>
#include <device/storage/ata.h>
#include <proc/sched.h>
#include <proc/syscall.h>
#include <fs/vfs.h>
#include <fs/ramfs.h>
#include <test/test.h>
#include <proc/elf.h>

static volatile struct limine_framebuffer_request fb_request = { 
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0 
};

static volatile struct limine_memmap_request mm_request = { 
    .id = LIMINE_MEMMAP_REQUEST,
    .revision = 0 
};

static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0
};

static volatile struct limine_rsdp_request rsdp_request = { 
    .id = LIMINE_RSDP_REQUEST,
    .revision = 0 
};

static volatile struct limine_kernel_address_request kernel_addr_request = {
    .id = LIMINE_KERNEL_ADDRESS_REQUEST,
    .revision = 0
};

static volatile struct limine_module_request module_request = { 
    .id = LIMINE_MODULE_REQUEST,
    .revision = 0 
};

_Noreturn void kcursor(task_id_t tid)
{
    while (true) {
        sleep(500);
        if (cursor_visible == CURSOR_INVISIBLE) {
            term_set_cursor('_');
            cursor_visible = CURSOR_VISIBLE;
        } else if (cursor_visible == CURSOR_VISIBLE) {
            term_set_cursor(' ');
            cursor_visible = CURSOR_INVISIBLE;
        } else {
            term_set_cursor(' ');
        }
        term_refresh(TERM_MODE_CLI, false);
    }   

    (void)tid;
}

void done(void)
{
    for (;;) {
        asm volatile ("hlt;");
    }   
}

/* This is HanOS kernel's entry point. */
void kmain(void)
{
    cpu_init();

    serial_init();
    klog_init();
    klogi("HanOS version 0.1 starting...\n");

    if (hhdm_request.response != NULL) {
        klogi("HHDM offset 0x%x, revision %d\n",
             hhdm_request.response->offset, hhdm_request.response->revision);
    }

    if (fb_request.response == NULL
        || fb_request.response->framebuffer_count < 1) {
        goto exit;  
    }   

    struct limine_framebuffer* fb =
        fb_request.response->framebuffers[0];

    term_init(fb);
    klogi("Framebuffer address: 0x%x\n", fb->address);

    gdt_init(NULL);
    idt_init();

    pmm_init(mm_request.response);
    vmm_init(mm_request.response, kernel_addr_request.response);

    term_start();

    /* Need to init after idt_init() because it will be used very often. */
    pit_init();
    keyboard_init();

    acpi_init(rsdp_request.response);
    hpet_init();
    cmos_init(); 
    apic_init();

    vfs_init();
    smp_init();
    syscall_init();

    struct limine_module_response *module_response = module_request.response;
    if (module_response != NULL) {
        for (size_t i = 0; i < module_response->module_count; i++) {
            struct limine_file *module = module_response->modules[i];
            klogi("Module %d path   : %s\n", i, module->path);
            klogi("Module %d cmdline: %s\n", i, module->cmdline);
            klogi("Module %d size   : %d\n", i, module->size);
            if (strcmp(module->cmdline, "INITRD") == 0) {
                ramfs_init(module->address, module->size);
            }
        }
    } else {
        kpanic("Cannot find INITRD module\n");
    } 

    klogi("Press \"\e[37m%s\e[0m\" (left) to shell and \"\e[37m%s\e[0m\" back\n",
          "ctrl+shift+1", "ctrl+shift+2");
#if 0
    int val1 = 10000, val2 = 0;
    kloge("Val: %d", val1 / val2);
#endif

    if (fb->edid_size == sizeof(edid_info_t)) {
        edid_info_t* edid = (edid_info_t*)fb->edid;
        klogi("EDID: version %d.%d, screen size %dcm * %dcm\n", edid->edid_version, edid->edid_revision,
              edid->max_hor_size, edid->max_ver_size);
        if (edid->dpms_flags & 0x02) {
            klogi("EDID: Preferred timing mode specified in DTD-1\n");
            klogi("EDID: %d * %d\n",
                  (uint16_t)edid->det_timings[0].horz_active +
                      (uint16_t)((uint16_t)(edid->det_timings[0].horz_active_blank_msb &
                      0xF0) << 4),
                  (uint16_t)edid->det_timings[0].vert_active +
                      (uint16_t)((uint16_t)(edid->det_timings[0].vert_active_blank_msb &
                      0xF0) << 4));
        }
    }   
    klogi("Framebuffer address 0x%x\n", fb->address);

#if 0
    task_t *tshell = sched_add(kshell, true);
    (void)tshell;
#else
    task_t *tshell = sched_add(NULL, true);
    auxval_t aux = {0};
    elf_load(tshell, DEFAULT_SHELL_APP, &aux);
    
    task_regs_t *tshell_regs = (task_regs_t*)tshell->tstack_top;
    tshell_regs->rip = (uint64_t)aux.entry;
#endif

    pci_init();
    ata_init();
    pci_get_gfx_device(kernel_addr_request.response);

#if 0
    file_test();
#endif

    klog_debug();
    fb_debug();

    sched_add(kcursor, false);
    term_clear(TERM_MODE_CLI);

    cpu_t *cpu = smp_get_current_cpu(false);
    if(cpu != NULL) {
        sched_init(cpu->cpu_id);
    } else {
        kpanic("Can not get CPU info in shell process\n");
    }

    goto exit;
    /* According to current implementation, the below codes will not be
     * executed.
     */
exit:
    done();
}
