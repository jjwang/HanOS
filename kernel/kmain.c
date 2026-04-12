/**-----------------------------------------------------------------------------

 @file    kmain.c
 @brief   Entry function of HanOS kernel
 @details
 @verbatim

  This function initializes various components of the operating system, such as
  the CPU, serial communication, logging, memory management, interrupt handling,
  ACPI, HPET, CMOS, APIC, PIT, keyboard, VFS, SMP, syscall, INITRD, and terminal.

  It also sets up the background image, prints system information, and starts
  the kupdateui task.

  Finally, it executes the default shell application.

  History:
    Feb 19, 2022  Added CLI task which supports some simple commands.
    May 21, 2022  Changed boot protocol to limine with corresponding
                  modifications.
    Jul 13, 2024  Added SLAB-based memory allocator.

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
#include <mm/mm.h>
#include <mm/alloc.h>
#include <sys/gdt.h>
#include <sys/idt.h>
#include <sys/isr_base.h>
#include <sys/smp.h>
#include <sys/mtrr.h>
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
#include <fs/filebase.h>
#include <fs/ramfs.h>
#include <fs/ttyfs.h>
#include <fs/pipefs.h>
#include <proc/elf.h>

LIMINE_BASE_REVISION(1)
static volatile struct limine_framebuffer_request fb_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0
};

static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0
};

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

static volatile computer_info_t self_info = { 0 };

extern addrspace_t kaddrspace;

void done(void)
{
    for (;;) {
        asm volatile ("hlt;");
    }
}

vec_new_static(char *, messages_info);
static lock_t messages_info_lock = lock_new();

void kdisplay(char *s, uint64_t len)
{
    (void)len;

    /* Here we just store the string into the temporary buffer */
    lock_lock(&messages_info_lock);
    vec_push_back(&messages_info, s);
    lock_release(&messages_info_lock);
}

_Noreturn void kupdateui(task_id_t tid)
{
    uint64_t last_ms = (hpet_get_nanos() / 1000000) % 1000;

    while (true) {
        uint64_t now_ms = (hpet_get_nanos() / 1000000) % 1000;

        if (now_ms - last_ms <= 500) {
            if (vec_length(&messages_info) > 0) {
                lock_lock(&messages_info_lock);
                char *s = vec_at(&messages_info, 0);
                vec_erase(&messages_info, 0);
                lock_release(&messages_info_lock);

                for (uint64_t i = 0; ; i++) {
                    if (s[i] == '\0') break;
                    serial_write(s[i]);
                }

                kmfree(s);
            }

            sched_sleep(0);

            continue;
        }

        last_ms = now_ms;

        if (cursor_visible == CURSOR_INVISIBLE) {
            term_set_cursor('_');
            cursor_visible = CURSOR_VISIBLE;
        } else if (cursor_visible == CURSOR_VISIBLE) {
            term_set_cursor(' ');
            cursor_visible = CURSOR_INVISIBLE;
        } else {
            term_set_cursor(' ');
        }

        term_refresh();
    }

    (void) tid;
}

_Noreturn void kshell(task_id_t tid)
{
    (void) tid;

    /* If we want to trigger an exception, uncomment below code */
    /* TODO: note that below codes cannot print exception messages. We need to
     * find out the reason.
     */
    if (false) {
        int y = 0, x = 128, z;
        z = x / y;
        klogi("kshell: 128 / 0 = %ld\n", z);
    }

    ttyfs_init();
    pipefs_init();

    ata_init();

#if 0                           /* Do not show desktop bitmap to speed up */
    image_t image;
    if (bmp_load_from_file(&image, "/assets/desktop.bmp")) {
        klogi("Background image: %ld*%ld with bpp %ld, size %ld\n",
              image.img_width, image.img_height, image.bpp, image.size);
        term_set_bg_image(&image);
    }
#endif

    term_refresh();

    kprintf
        ("General Purpose OS based on HNK kernel version %s. Copyleft (2024) HNK.\n",
         VERSION);

    char *cpu_model_name = cpu_get_model_name();
    if (strlen(cpu_model_name) > 0) {
        kprintf("\033[36mCPU        \033[0m: %s\n", cpu_model_name);
    }

    {
        kprintf("\033[36mMemory     \033[0m: %11d MB\n",
                pmm_get_total_memory());
    }

    if (self_info.screen_hor_size > 0 && self_info.screen_ver_size > 0) {
        kprintf("\033[36mMonitor    \033[0m: %4d x %4d cm\n",
                self_info.screen_hor_size, self_info.screen_ver_size);
    }

    if (self_info.prefer_res_x > 0 && self_info.prefer_res_y > 0) {
        kprintf("\033[36mPreferred  \033[0m: %4d x %4d Pixels\n",
                self_info.prefer_res_x, self_info.prefer_res_y);
    }

    if (self_info.actual_res_x > 0 && self_info.actual_res_y > 0) {
        kprintf("\033[36mActual     \033[0m: %4d x %4d Pixels\n",
                self_info.actual_res_x, self_info.actual_res_y);
    }

    /* Start all programs */
#if ENABLE_BASH
    const char *argv[] = { "/usr/bin/bash", "--login", NULL };
    const char *envp[] = {
        "HOME=/root",
        "TIME_STYLE=posix-long-iso",
        "PATH=/usr/bin:/bin",
        "SHELL=/usr/bin/bash",
        "TERM=xterm-color",
        NULL
    };

    sched_execve(DEFAULT_SHELL_APP, argv, envp, "/root");
#else
    sched_execve(DEFAULT_SHELL_APP, NULL, NULL, "/root");
#endif

    /* This should be idle task which frees resources of all dead tasks */
    task_t *t = sched_get_current_task();
    if (t != NULL) {
        task_idle_proc(t->tid);
    } else {
        while (true) {
            done();
        }
    }
}

/* This is HanOS kernel's entry point. */
void kmain(void)
{
    serial_init();

    klog_init();

    idt_init();
    cpu_init(0);

    klogi("HanOS version %s starting...\n", VERSION);

    if (hhdm_request.response != NULL) {
        klogi("HHDM offset 0x%016lx, revision %ld\n",
              hhdm_request.response->offset,
              hhdm_request.response->revision);
    } else {
        kpanic("HHDM is NULL\n");
    }

    if (fb_request.response == NULL) {
        goto exit;
    } else if (fb_request.response->framebuffer_count < 1) {
        goto exit;
    }

    struct limine_framebuffer *fb = fb_request.response->framebuffers[0];
    if (fb->width > FB_WIDTH || fb->height > FB_HEIGHT) {
        /* Resolution cannot be supported */
        done();
    }

    term_init(fb);

    klogi("Framebuffer address: 0x%016lx, EDID size: %ld\n",
          fb->address, fb->edid_size);

    klogi("Init CMOS...\n");
    cmos_init();

    gdt_init(NULL);

#if USE_BUDDY_ALLOCATOR
    pmm_register_buddy_allocator();
#else
    pmm_register_bitmap_allocator();
#endif
    pmm_init(mm_request.response, hhdm_request.response->offset);
    alloc_init();

    vmm_init(mm_request.response, kernel_addr_request.response);

    /* Below code will cause #PF in term_clear() */
    /*
     * mtrr_save(0, (void *) VIRT_TO_PHYS(fb->address));
     * mtrr_restore(0);
     */

    term_start();

    klogi("Init PIT...\n");
    pit_init();

    klogi("Init keyboard...\n");
    keyboard_init();

    klogi("Init ACPI...\n");
    acpi_init(rsdp_request.response);

    klogi("Init HPET...\n");
    hpet_init();

    klogi("Init PCI...\n");
    pci_init();
    gfx_init();

    klogi("Init APIC...\n");
    apic_init();

    klogi("Init syscall...\n");
    syscall_init();

    if (fb->edid_size == sizeof(edid_info_t)) {
        edid_info_t *edid = (edid_info_t *) fb->edid;
        klogi("EDID: version %ld.%ld, screen size %dcm * %dcm\n",
              edid->edid_version, edid->edid_revision, edid->max_hor_size,
              edid->max_ver_size);

        self_info.screen_hor_size = edid->max_hor_size;
        self_info.screen_ver_size = edid->max_ver_size;

        self_info.prefer_res_x =
            (uint16_t) edid->det_timings[0].horz_active +
            (uint16_t) ((uint16_t)
                        (edid->
                         det_timings[0].horz_active_blank_msb & 0xF0) <<
                        4);
        self_info.prefer_res_y =
            (uint16_t) edid->det_timings[0].vert_active +
            (uint16_t) ((uint16_t)
                        (edid->
                         det_timings[0].vert_active_blank_msb & 0xF0) <<
                        4);

        if (edid->dpms_flags & 0x02) {
            klogi("EDID: Preferred timing mode specified in DTD-1\n");
            klogi("EDID: %ld * %ld\n",
                  self_info.prefer_res_x, self_info.prefer_res_y);
        }
    } else if (fb->edid_size == 0) {
        klogi("Framebuffer: totally %ld video modes\n", fb->mode_count);
        for (uint64_t i = 0; i < fb->mode_count; i++) {
            struct limine_video_mode *mode = fb->modes[i];
            klogd
                ("             %ld (width) * %ld (height), %ld (bpp), %ld (pitch)\n",
                 mode->width, mode->height, mode->bpp, mode->pitch);
        }
    }

    klogi("Framebuffer address 0x%016lx\n", fb->address);

    self_info.actual_res_x = fb->width;
    self_info.actual_res_y = fb->height;

    vfs_init();

    /* Register keyboard as /dev/kbd char device */
    vfs_tnode_t *kbd_tnode =
        vfs_path_to_node("/dev/kbd", CREATE, VFS_NODE_CHAR_DEVICE);
    vfs_inode_t *kbd_inode =
        vfs_alloc_inode(VFS_NODE_CHAR_DEVICE, 0600, 0, NULL, kbd_tnode);
    kbd_tnode->inode = kbd_inode;
    kbd_inode->ident = (void *) keyboard_get_char_device_ops();

    klogi("Init SMP...\n");
    smp_init();

    /* Measure pure CPU computation speed by running a simple busy loop of
     * 100 million no-op operations.
     */
    uint64_t start, end;

    /* This is used to verify that CPU and memory performance are normal on
     * the hardware.
     *
     * K29 QEMU    : 93363090 nano seconds
     * NEC VersaPro: 83394533 nano seconds
     */
    start = hpet_get_nanos();
    for (int64_t ii = 0; ii < 100000000; ++ii) asm volatile("" ::: "memory");
    end = hpet_get_nanos();
    klogi("Empty loop cost      : %ld nano seconds\n", end - start);

    /* Framebuffer write benchmark removed: aperture is uncached MMIO on real
     * hardware, so 100M writes take ~7.6s and stall the scheduler. */

    klogi("Init INITRD...\n");
    struct limine_module_response *module_response =
        module_request.response;
    if (module_response != NULL) {
        for (uint64_t i = 0; i < module_response->module_count; i++) {
            struct limine_file *module = module_response->modules[i];
            klogi("Module %ld path   : %s\n", i, module->path);
            klogi("Module %ld cmdline: %s\n", i, module->cmdline);
            klogi("Module %ld size   : %ld\n", i, module->size);
            if (strcmp(module->cmdline, "INITRD") == 0) {
                /* If we do not call vmm_map() here, there will be Page Fault
                 * exception on real hardware when building in gcc and other
                 * tools.
                 */
                vmm_map(&kaddrspace, (uint64_t) module->address,
                        VIRT_TO_PHYS(module->address),
                        NUM_PAGES(module->size), VMM_FLAGS_DEFAULT);
                ramfs_init(module->address, module->size);
            }
        }
    } else {
        kpanic("Cannot find INITRD module\n");
    }

    klog_debug();

    task_t *tupdateui = sched_new("kupdateui", kupdateui, false);
    sched_add(tupdateui);

    term_clear();

    task_t *tshell = sched_new("kshell", kshell, false);
    sched_add(tshell);

    cpu_t *cpu = smp_get_current_cpu(false);
    if (cpu != NULL) {
        sched_init("idle", cpu->cpu_id);
        apic_timer_start();
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
