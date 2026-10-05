/**-----------------------------------------------------------------------------

 @file    kmain.c
 @brief   Entry function of HanOS kernel
 @details
 @verbatim

  This function initializes various components of the operating system, such as
  the CPU, serial communication, logging, memory management, interrupt handling,
  ACPI, HPET, CMOS, APIC, PIT, input, VFS, SMP, syscall, and INITRD.

  It also prints system information and starts the userspace servers.

  Finally, it executes the default shell application.

  History:
    Feb 19, 2022  Added CLI process which supports some simple commands.
    May 21, 2022  Changed boot protocol to limine with corresponding
                  modifications.
    Jul 13, 2024  Added SLAB-based memory allocator.

@endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdint.h>

#include <stddef.h>

#include <string.h>

#include <kconfig.h>
#include <version.h>
#include <3rd-party/boot/limine.h>
#include <lib/time.h>
#include <lib/klog.h>
#include <mm/mm.h>
#include <mm/alloc.h>
#include <arch/x64/gdt.h>
#include <arch/x64/idt.h>
#include <arch/x64/isr_base.h>
#include <arch/x64/smp.h>
#include <arch/x64/mtrr.h>
#include <arch/x64/cmos.h>
#include <arch/x64/serial.h>
#include <arch/x64/acpi.h>
#include <arch/x64/apic.h>
#include <arch/x64/ioapic.h>
#include <arch/x64/hpet.h>
#include <arch/x64/panic.h>
#include <arch/x64/pci.h>
#include <arch/x64/pit.h>
#include <arch/x64/timer.h>
#include <device/display/fb.h>
#include <device/display/edid.h>
#include <device/display/display_mode.h>
#include <device/display/gfx.h>
#include <device/usb/xhci.h>
#include <srv/block_srv.h>
#include <srv/vfs_srv.h>
#include <srv/pipe_srv.h>
#include <srv/process_srv.h>
#include <srv/net_srv.h>
#include <srv/tty_srv.h>
#include <srv/fat32_srv.h>
#include <srv/ext2_srv.h>
#include <proc/sched.h>
#include <proc/syscall.h>
#include <proc/notify.h>
#include <srv/input_srv.h>
#include <srv/console_srv.h>
#include <srv/svc_monitor.h>
#include <ipc/selftest.h>
#include <fs/vfs.h>
#include <fs/initrd.h>
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

_Noreturn void kshell(pid_t pid)
{
    (void) pid;

    /* If we want to trigger an exception, uncomment below code */
    /* TODO: Note that the code below cannot print exception messages; find
     * out why.
     */
    if (false) {
        int32_t y = 0, x = 128, z;
        z = x / y;
        klogi("kshell: 128 / 0 = %ld\n", z);
    }

    if (!console_server_start())
        klogw("console: server failed to start\n");

    kprintf
        ("General Purpose OS based on HNK kernel version %s. Copyleft (2024) HNK.\n",
         VERSION);

    char *cpu_model_name = cpu_get_model_name();
    if (strlen(cpu_model_name) > 0) {
        kprintf("\033[36mCPU Model   \033[0m: %s\n", cpu_model_name);
    }

    {
        kprintf("\033[36mMemory      \033[0m: %d MB\n",
                pmm_get_total_memory());
    }

    {
        pci_device_t gpu;

        if (pci_find_class(PCI_CLASS_DISPLAY, 0x00, &gpu)
            || pci_find_class(PCI_CLASS_DISPLAY, 0x80, &gpu)) {
            kprintf("\033[36mVideo Card  \033[0m: %s\n",
                    pci_device_id_to_string(&gpu));
        }
    }

    if (self_info.screen_hor_size > 0 && self_info.screen_ver_size > 0) {
        kprintf("\033[36mMonitor     \033[0m: %d x %d cm\n",
                self_info.screen_hor_size, self_info.screen_ver_size);
    }

    if (self_info.actual_res_x > 0 && self_info.actual_res_y > 0) {
        kprintf("\033[36mResolution  \033[0m: %d x %d Pixels\n",
                self_info.actual_res_x, self_info.actual_res_y);
    }

    {
        pci_device_t audio;

        if (pci_find_class_any(PCI_CLASS_MULTIMEDIA, &audio)) {
            kprintf("\033[36mAudio Card  \033[0m: %s\n",
                    pci_device_id_to_string(&audio));
        }
    }

    {
        pci_device_t nic;

        if (pci_find_class(PCI_CLASS_NETWORK, 0x00, &nic)) {
            kprintf("\033[36mNet Card    \033[0m: %s\n",
                    pci_device_id_to_string(&nic));
        }
    }

    {
        pci_device_t disk;

        if (pci_find_class_any(PCI_CLASS_STORAGE, &disk)) {
            kprintf("\033[36mHard Disk   \033[0m: %s\n",
                    pci_device_id_to_string(&disk));
        }
    }

    {
        pci_device_t usb;

        if (pci_find_class(PCI_CLASS_SERIAL_BUS, 0x03, &usb)) {
            kprintf("\033[36mU-Disk      \033[0m: %s\n",
                    pci_device_id_to_string(&usb));
        }
    }

    /* Start all programs. Boot order: console -> input -> tty -> block ->
     * fat32 -> vfs -> pipe -> init, so each server's dependency (tty on the
     * console endpoint, fat32 on the block server) is up first. */
    if (!input_server_start())
        klogw("input: server failed to start\n");
    if (!tty_server_start())
        klogw("tty: server failed to start\n");
    if (!block_server_start())
        klogw("block: server failed to start\n");
    else
        block_server_probe();
    if (!block_server_active())
        klogw("fat32: no block server, skipping\n");
    else if (!fat32_server_start())
        klogw("fat32: server failed to start\n");
    else
        fat32_server_probe();
    if (!block_server_active())
        klogw("ext2: no block server, skipping\n");
    else if (!ext2_server_start())
        klogw("ext2: server failed to start\n");
    if (!vfs_server_start())
        klogw("vfs: server failed to start\n");
    else {
        vfs_server_probe();
        if (ext2_server_active())
            ext2_server_probe();
    }
    if (!pipe_server_start())
        klogw("pipe: server failed to start\n");
    if (!process_server_start())
        klogw("process: server failed to start\n");
    else
        process_server_probe();
    if (!net_server_start())
        klogw("net: server failed to start\n");

    /* Restart a server when its process dies. */
    svc_monitor_start();
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

    /* This should be idle process which frees resources of all dead processes */
    process_t *t = sched_get_current_process();
    if (t != NULL) {
        process_idle(t->pid);
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

    /* The framebuffer geometry comes from the bootloader (limine.conf), so all
     * buffers are sized from the reported width/height/pitch rather than a
     * fixed maximum. */
    fb_init(fb_get(), fb);

    /* Cover the screen as early as possible: the bootloader blanks the
     * framebuffer when it hands over, and the userspace console server cannot
     * run until the scheduler and the initrd are ready. */
    fb_splash(fb_get());

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

    /* The notification object is built on IPC endpoints, so initialise it
     * once the allocator is up and before any driver can publish. */
    notify_system_init();

    vmm_init(mm_request.response, kernel_addr_request.response);

    klogi("Init PIT...\n");
    pit_init();

    klogi("Init ACPI...\n");
    acpi_init(rsdp_request.response);

    klogi("Init HPET...\n");
    hpet_init();

    klogi("Init PCI...\n");
    pci_init();

    /* Build the display timing model before the GPU driver so it can program
     * the pipeline at the boot resolution (EDID preferred, else the current
     * framebuffer geometry). */
    display_mode_t display_mode;
    if (fb->edid_size == sizeof(edid_info_t)
        && display_mode_from_edid((edid_info_t *) fb->edid, &display_mode)) {
        display_mode_log(&display_mode);
    } else {
        display_mode_from_fb(&display_mode, (uint32_t) fb->width,
                             (uint32_t) fb->height, (uint32_t) fb->pitch);
        display_mode_log(&display_mode);
    }

    gfx_init();

    /* Redraw the splash in case the display driver blanked the screen with its
     * mode set, then fill the progress bar as the remaining boot steps run. */
    fb_splash(fb_get());

    klogi("Init APIC...\n");
    apic_init();

    /* Route the legacy lines through the I/O APIC now that the local APIC is
     * up, and move the PIT line off the 8259. */
    ioapic_init();
    irq_clear_mask(0);

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

    /* Report the geometry the display is actually scanning: the driver may
     * have switched to the panel's native mode after taking over. */
    fb_info_t *fb_info = fb_get();
    self_info.actual_res_x = fb_info->width;
    self_info.actual_res_y = fb_info->height;

    klogi("Init SMP...\n");
    fb_splash_progress(fb_get(), 20);
    smp_init();
    fb_splash_progress(fb_get(), 55);

    /* Bring the USB pointer up in its own thread: the enumeration has timeouts
     * and must never block the rest of the boot. */
    usb_hid_start();

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
    fb_splash_progress(fb_get(), 75);
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
                initrd_init(module->address, module->size);
            }
        }
    } else {
        kpanic("Cannot find INITRD module\n");
    }
    fb_splash_progress(fb_get(), 100);

    process_t *tshell = sched_new("kshell", kshell, false);
    sched_add(tshell);

#if ENABLE_MICROKERNEL_SELFTEST
    process_t *tmktest = sched_new("mktest", mk_selftest_process, false);
    sched_add(tmktest);
#endif

    cpu_t *cpu = smp_get_current_cpu(false);
    if (cpu != NULL) {
        /* The boot thread is not a schedulable process, so an interrupt that
         * reaches do_context_switch before this core's timer is running would
         * park it in the idle loop forever. Keep interrupts off until the
         * timer can schedule the core. */
        asm volatile ("cli" ::: "memory");
        sched_init("idle", cpu->cpu_id);
        apic_timer_start();
        asm volatile ("sti" ::: "memory");
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
