/**-----------------------------------------------------------------------------

 @file    kmain.c
 @brief   Entry function of HanOS kernel
 @details
 @verbatim

  This function initializes various components of the operating system, such as
  the CPU, serial communication, logging, memory management, interrupt handling,
  ACPI, HPET, CMOS, APIC, PIT, keyboard, VFS, SMP, syscall, INITRD, and terminal.

  It also sets up the background image, prints system information, and starts
  the kcursor task.

  Finally, it executes the default shell application.

  History:
    Feb 19, 2022  Added CLI task which supports some simple commands.
    May 21, 2022  Changed boot protocol to limine with corresponding
                  modifications.

@endverbatim

 **-----------------------------------------------------------------------------
 */

#include <stddef.h>

#include <libc/string.h>

#include <kconfig.h>
#include <version.h>
#include <3rd-party/boot/limine.h>
#include <base/time.h>
#include <base/image.h>
#include <base/klog.h>
#include <sys/mm.h>
#include <sys/gdt.h>
#include <sys/idt.h>
#include <sys/isr_base.h>
#include <sys/smp.h>
#include <sys/cmos.h>
#include <sys/serial.h>
#include <sys/acpi.h>
#include <sys/apic.h>
#include <sys/hpet.h>
#include <sys/panic.h>
#include <sys/pci.h>
#include <sys/pit.h>
#include <device/display/fb.h>
#include <device/display/term.h>
#include <device/display/edid.h>
#include <device/display/gfx.h>
#include <device/keyboard/keyboard.h>
#include <device/storage/ata.h>
#include <proc/sched.h>
#include <proc/syscall.h>
#include <fs/vfs.h>
#include <fs/ramfs.h>
#include <fs/ttyfs.h>
#include <proc/elf.h>

#if 1
static volatile struct limine_framebuffer_request fb_request = { 
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0 
};

static volatile struct limine_hhdm_request hhdm_request = { 
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0 
};
#endif

static volatile struct limine_memmap_request mm_request = { 
    .id = LIMINE_MEMMAP_REQUEST,
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

static volatile struct limine_terminal_request terminal_request = {
    .id = LIMINE_TERMINAL_REQUEST,
    .revision = 0
};

static volatile computer_info_t self_info = {0};

_Noreturn void kcursor(task_id_t tid)
{
    while (true) {
        sched_sleep(500);

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

void done(void)
{
    for (;;) {
        asm volatile ("hlt;");
    }   
}

void screen_write(char c)
{
    (void)c;
#if !(LAUNCHER_GRAPHICS)
    char s[2] = {0};

    s[0] = c;
    s[1] = '\0';

    if (terminal_request.response == NULL
        || terminal_request.response->terminal_count < 1) {
        done();
    }   

    struct limine_terminal *terminal = terminal_request.response->terminals[0];
    terminal_request.response->write(terminal, s, 1);
#endif
}

/* This is HanOS kernel's entry point. */
void kmain(void)
{
    /* Limine sanity check */
    if (terminal_request.response == NULL
        || terminal_request.response->terminal_count < 1) {
        done();
    }
    struct limine_terminal *terminal = terminal_request.response->terminals[0];
    terminal_request.response->write(terminal, "Starting HanOS...\n", 18);

    cpu_init();

    serial_init();
    klog_init();
    klogi("HanOS version %s starting...\n", VERSION);

#if LAUNCHER_GRAPHICS
    if (hhdm_request.response != NULL) {
        klogi("HHDM offset 0x%x, revision %d\n",
             hhdm_request.response->offset, hhdm_request.response->revision);
    }

    if (fb_request.response == NULL) {
        goto exit;
    } else if (fb_request.response->framebuffer_count < 1) {
        goto exit;  
    }

    struct limine_framebuffer *fb =
        fb_request.response->framebuffers[0];
    if (fb->width > FB_WIDTH || fb->height > FB_HEIGHT) {
        char *err_msg = "Resolution cannot be supported";
        terminal_request.response->write(terminal, err_msg, strlen(err_msg));
        done();
    }

    term_init(fb);

    klogi("Framebuffer address: 0x%x\n", fb->address);
#else
    klogi("Terminal: rows %d, columns %d, framebuffer 0x%x (0x%x)\n",
          terminal->rows, terminal->columns,
          terminal->framebuffer, terminal->framebuffer->address);
    term_init(NULL);
#endif

    gdt_init(NULL);
    idt_init();

    pmm_init(mm_request.response);
    vmm_init(mm_request.response, kernel_addr_request.response);

    term_start();

    klogi("Init PIT...\n");
    pit_init();

    klogi("Init keyboard...\n");
    keyboard_init();

    klogi("Init ACPI...\n");
    acpi_init(rsdp_request.response);

    klogi("Init HPET...\n");
    hpet_init();

    klogi("Init CMOS...\n");
    cmos_init();

    klogi("Init APIC...\n");
    apic_init();

    klogi("Init VFS...\n");
    vfs_init();

    klogi("Init SMP...\n");
    smp_init();

    klogi("Init syscall...\n");
    syscall_init();

    klogi("Init INITRD...\n");
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

    ttyfs_init(); 

    klogi("Press \"\033[37m%s\033[0m\" (left) to shell and \"\033[37m%s\033[0m\" back\n",
          "ctrl+shift+1", "ctrl+shift+2");

#if LAUNCHER_GRAPHICS
    if (fb->edid_size == sizeof(edid_info_t)) {
        edid_info_t* edid = (edid_info_t*)fb->edid;
        klogi("EDID: version %d.%d, screen size %dcm * %dcm\n", edid->edid_version, edid->edid_revision,
              edid->max_hor_size, edid->max_ver_size);

        self_info.screen_hor_size = edid->max_hor_size;
        self_info.screen_ver_size = edid->max_ver_size;

        self_info.prefer_res_x = (uint16_t)edid->det_timings[0].horz_active +
            (uint16_t)((uint16_t)(edid->det_timings[0].horz_active_blank_msb &
            0xF0) << 4);
        self_info.prefer_res_y = (uint16_t)edid->det_timings[0].vert_active +
            (uint16_t)((uint16_t)(edid->det_timings[0].vert_active_blank_msb &
            0xF0) << 4);

        if (edid->dpms_flags & 0x02) {
            klogi("EDID: Preferred timing mode specified in DTD-1\n");
            klogi("EDID: %d * %d\n",
                  self_info.prefer_res_x, self_info.prefer_res_y);
        }
    }
    klogi("Framebuffer address 0x%x\n", fb->address);
#endif

    pci_init();
    ata_init();

    pci_get_gfx_device(kernel_addr_request.response);

    image_t image;
    if (bmp_load_from_file(&image, "/assets/desktop.bmp")) {
        klogi("Background image: %d*%d with bpp %d, size %d\n",
              image.img_width, image.img_height, image.bpp, image.size);
        term_set_bg_image(&image);
    }

    klog_debug();

    task_t *tcursor = sched_new("kcursor", kcursor, false);
    sched_add(tcursor);

#if LAUNCHER_CLI
    term_clear(TERM_MODE_CLI);
#endif

    kprintf("HanOS based on HNK kernel version %s. Copyleft (2022) HNK.\n",
            VERSION); 

    char *cpu_model_name = cpu_get_model_name();
    if (strlen(cpu_model_name) > 0) {
        kprintf("\033[36mCPU        \033[0m: %s\n", cpu_model_name);
    }

    {
        kprintf("\033[36mMemory     \033[0m: %11d MB\n", pmm_get_total_memory());
    }

    if (self_info.screen_hor_size > 0 && self_info.screen_ver_size > 0) {
        kprintf("\033[36mMonitor    \033[0m: %4d x %4d cm\n",
                self_info.screen_hor_size, self_info.screen_ver_size);
    }

    if (self_info.screen_hor_size > 0 && self_info.screen_ver_size > 0) {
        kprintf("\033[36mPreferred  \033[0m: %4d x %4d Pixels\n",
                self_info.prefer_res_x, self_info.prefer_res_y);
#if LAUNCHER_GRAPHICS
        kprintf("\033[36mActual     \033[0m: %4d x %4d Pixels\n",
                fb->width, fb->height);
#endif
    }

    /* Start all programs */
#ifdef ENABLE_BASH
    const char *argv[] = { "/usr/bin/bash", "--login", NULL };
    const char *envp[] = { 
        "HOME=/root",
        "TIME_STYLE=posix-long-iso",
        "PATH=/usr/bin:/bin",
        "TERM=hanos",
        NULL
    };

    sched_execve(DEFAULT_SHELL_APP, argv, envp, "/root"); 
#else 
    sched_execve(DEFAULT_SHELL_APP, NULL, NULL, "/root");
#endif

    cpu_t *cpu = smp_get_current_cpu(false);
    if(cpu != NULL) {
        sched_init("init", cpu->cpu_id);
    } else {
        kpanic("Can not get CPU info in shell process\n");
    }

    /* According to current implementation, the below codes will not be
     * executed.
     */
    goto exit;
exit:
    done();
}
