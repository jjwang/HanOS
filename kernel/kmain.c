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
#include <fs/vfs.h>
#include <test/test.h>

static volatile struct limine_terminal_request terminal_request = {
    .id = LIMINE_TERMINAL_REQUEST,
    .revision = 0
};

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

static volatile enum {
    CURSOR_INVISIBLE = 0,
    CURSOR_VISIBLE,
    CURSOR_HIDE
} cursor_visible = CURSOR_INVISIBLE;

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
        term_refresh(TERM_MODE_CLI);
    }   

    (void)tid;
}

static volatile char current_dir[VFS_MAX_PATH_LEN] = {0};

_Noreturn void kshell(task_id_t tid)
{
    (void)tid;
    command_t cmd_list[] = {
        {"memory", pmm_dump_usage, "Dump memory usage information"},
        {"vfs",    vfs_debug,      "Dump tree of virtual file system"},
        {"pci",    pci_debug,      "Print list of PCI devices"},
        {"ls",     dir_test,       "List all files of current directory"},
        {"",       NULL,           ""}};

    klogi("Shell task started\n");

    kprintf("?[11;1m%s?[0m\n\n", "Hello World");
    kprintf("?[11;1m%s?[0m Type \"?[14;1m%s?[0m\" for command list\n",
            "Welcome to HanOS world!", "help");
    kprintf("?[14;1m%s?[0m", "$ ");

    pci_init();
#if 0
    ata_init();
#endif

    pci_get_gfx_device(kernel_addr_request.response);

    char cmd_buff[1024] = {0};
    uint16_t cmd_end = 0;

    while (true) {
        sleep(16);
        uint8_t cur_key = keyboard_get_key();
        if (term_get_mode() != TERM_MODE_CLI) {
            continue;
        }
        if (cur_key == 0x0A) {
            cursor_visible = CURSOR_HIDE;
            term_set_cursor(' ');
            term_refresh(TERM_MODE_CLI);
            kprintf("%c", cur_key);

            if (cmd_end > 0) {
                if (strcmp(cmd_buff, "help") == 0) {
                    kprintf("%16s%s\n", "Command", "Description");
                    kprintf("%16s%s\n", "-------", "-----------");
                    for(int64_t i = 0; ; i++) {
                        if (cmd_list[i].proc == NULL) break;
                        kprintf("%16s%s\n", cmd_list[i].command, cmd_list[i].desc);
                    }
                } else {
                    for(int64_t i = 0; ; i++) {
                        if (cmd_list[i].proc == NULL) {
                            kprintf("Sorry, I cannot understand.\n");
                            break;
                        } else if (strcmp(cmd_buff, cmd_list[i].command) == 0) {
                            (cmd_list[i].proc)();
                            break;
                        }
                    }
                }
            }

            cursor_visible = CURSOR_INVISIBLE;
            cmd_buff[0] = '\0';
            cmd_end = 0;
            kprintf("?[14;1m%s?[0m", "$ ");
            cursor_visible = CURSOR_INVISIBLE;
        } else if (cur_key == '\b') {
            if (cmd_end > 0) {
                cmd_buff[cmd_end--] = '\0';
                kprintf("?[14;1m%c?[0m", cur_key);
            }   
        }  else if (cur_key > 0) {
            if (cmd_end < sizeof(cmd_buff) - 1) {
                cmd_buff[cmd_end++] = cur_key;
                cmd_buff[cmd_end] = '\0';
            }   
            kprintf("?[14;1m%c?[0m", cur_key);
        }
    }   
}

void done(void)
{
    for (;;) {
        asm volatile ("hlt;");
    }   
}

void kdisplay(char* s)
{
    if (terminal_request.response == NULL
        || terminal_request.response->terminal_count < 1) {
        done();
    }   

    struct limine_terminal *terminal = terminal_request.response->terminals[0];
    terminal_request.response->write(terminal, s, strlen(s));
}

/* This is HanOS kernel's entry point. */
void kmain(void)
{
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

    if (fb->edid_size == sizeof(edid_info_t)) {
        edid_info_t* edid = (edid_info_t*)fb->edid;
        klogi("EDID %d.%d: %d * %d\n", edid->edid_version, edid->edid_revision,
              edid->max_hor_size, edid->max_ver_size);
    }
    klogi("Framebuffer address 0x%x\n", fb->address);

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

    klogi("Press \"?[14;1m%s?[0m\" (left) to shell and \"?[14;1m%s?[0m\" back\n",
          "ctrl+shift+1", "ctrl+shift+2");
#if 0
    int val1 = 10000, val2 = 0;
    kloge("Val: %d", val1 / val2);
#endif

    sched_add(kshell);
    sched_add(kcursor);

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
