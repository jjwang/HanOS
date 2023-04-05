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
#include <version.h>
#include <3rd-party/boot/limine.h>
#include <lib/time.h>
#include <lib/image.h>
#include <lib/klog.h>
#include <lib/string.h>
#include <lib/shell.h>
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
#include <test/test.h>
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

    auxval_t aux = {0};
    uint64_t entry = 0;

    task_t *tshell = sched_new(NULL, true);
    elf_load(tshell, DEFAULT_SHELL_APP, &entry, &aux);
    task_regs_t *tshell_regs = (task_regs_t*)tshell->tstack_top;
    if (tshell->mode == TASK_USER_MODE) {
        tshell_regs = (task_regs_t*)PHYS_TO_VIRT(tshell_regs);
    }

    if (aux.entry != entry) {
        /* Need to handle dynamic linker */
        uint64_t *stack = (uint64_t*)tshell->tstack_top;
        if (tshell->mode == TASK_USER_MODE) {
            stack = (uint64_t*)PHYS_TO_VIRT(stack);
        }

#ifdef ENABLE_BASH
        strcpy(tshell->cwd, "/root");

        uint8_t *sa = (uint8_t*)stack;

        const char *argv[] = { "/usr/bin/bash", "--login", NULL };
        const char *envp[] = { 
            "HOME=/root",
            "PATH=/usr/local/bin:/usr/bin:/bin",
            "TERM=linux",
            NULL
        };

        size_t nenv = 0;
        for (const char **e = envp; *e; e++) {
            stack = (void*)stack - (strlen(*e) + 1);
            strcpy((char*)stack, *e);
            nenv++;
        }

        size_t nargs = 0;
        for (const char **e = argv; *e; e++) {
            stack = (void*)stack - (strlen(*e) + 1);
            strcpy((char*)stack, *e);
            nargs++;
        }

        /* Align stack address to 16-byte */
        stack = (void*)stack - ((uintptr_t)stack & 0xf);

        if ((nargs + nenv + 1) & 1)
            stack--;
#else
        *(--stack) = 0;
#endif

        /* Auxilary vector */
        *(--stack) = 0;
        *(--stack) = 0;

        stack   -= 2;
        stack[0] = 10;
        stack[1] = aux.entry;

        stack   -= 2;
        stack[0] = 20;
        stack[1] = aux.phdr;

        stack   -= 2;
        stack[0] = 21;
        stack[1] = aux.phentsize;

        stack   -= 2;
        stack[0] = 22;
        stack[1] = aux.phnum;

        klogi("Shell: tid %d aux stack 0x%x, entry 0x%x, phdr 0x%x, "
              "phentsize %d, phnum %d\n", tshell->tid, stack, aux.entry, aux.phdr,
              aux.phentsize, aux.phnum);

        /* Environment variables */
        *(--stack) = 0;     /* End of environment */
#ifdef ENABLE_BASH
        stack -= nenv;
        for (size_t i = 0; i < nenv; i++) {
            sa -= strlen(envp[i]) + 1;
            stack[i] = (uint64_t)sa; 
        }  
#endif

        /* Arguments */
        *(--stack) = 0;     /* End of arguments */
#ifdef ENABLE_BASH
        stack -= nargs;
        for (size_t i = 0; i < nargs; i++) {
            sa -= strlen(argv[i]) + 1;
            stack[i] = (uint64_t)sa;
        }
        *(--stack) = nargs; /* argc */
#else
        *(--stack) = 0;
#endif

        stack = (uint64_t*)((uint64_t)stack - sizeof(task_regs_t));
        memcpy(stack, tshell_regs, sizeof(task_regs_t));

        tshell->tstack_top = (void*)VIRT_TO_PHYS(stack);
        tshell_regs = (task_regs_t*)stack;
        tshell_regs->rsp = (uint64_t)tshell->tstack_top + sizeof(task_regs_t);
    }

    tshell_regs->rip = (uint64_t)entry;
    klogi("Shell: task stack 0x%x, PML4 0x%x, entry 0x%x\n", tshell->tstack_top,
        (tshell->addrspace == NULL) ? NULL : tshell->addrspace->PML4, entry);

#if 0 /* Should be commented when debuging mlibc */
    task_t *tkbd = sched_new(NULL, true);
    elf_load(tkbd, DEFAULT_INPUT_SVR, &entry, &aux);
    
    task_regs_t *tkbd_regs = (task_regs_t*)tkbd->tstack_top;
    tkbd_regs->rip = (uint64_t)entry;
    klogi("Keyboard: task stack 0x%x, PML4 0x%x, entry 0x%x\n", tkbd->tstack_top,
        (tkbd->addrspace == NULL) ? NULL : tkbd->addrspace->PML4, entry);
#endif

    pci_init();
    ata_init();

#if 0
    pci_get_gfx_device(kernel_addr_request.response);
#endif

    image_t image;
    if (bmp_load_from_file(&image, "/assets/desktop.bmp")) {
        klogi("Background image: %d*%d with bpp %d, size %d\n",
              image.img_width, image.img_height, image.bpp, image.size);
        term_set_bg_image(&image);
    }

    klog_debug();

    task_t *tcursor = sched_new(kcursor, false);
    sched_add(tcursor);

#if 0 /* Should be commented when debuging mlibc */
    sched_add(tkbd);
#endif

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
    sched_add(tshell);

    cpu_t *cpu = smp_get_current_cpu(false);
    if(cpu != NULL) {
        sched_init(cpu->cpu_id);
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
