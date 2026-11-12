/**-----------------------------------------------------------------------------

 @file    gpu_srv.c
 @brief   Spawn the userspace GPU server
 @details
 @verbatim

  The kernel discovers the Intel graphics device and maps its GTTMMADR and
  GMADR BARs into /bin/gpu. The server owns mode setting, scan-out buffers,
  the rain desktop and the hardware cursor. The kernel forwards pointer
  deltas to the same endpoint.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdint.h>
#include <string.h>

#include <bootinfo.h>
#include <kconfig.h>
#include <protocol.h>

#include <arch/x64/cpu.h>
#include <arch/x64/pci.h>
#include <device/display/display_mode.h>
#include <device/display/gfx.h>
#include <device/display/fb.h>
#include <ipc/ipc.h>
#include <lib/kmalloc.h>
#include <lib/klog.h>
#include <lib/vector.h>
#include <mm/mm.h>
#include <proc/sched.h>
#include <router/router.h>
#include <srv/gpu_srv.h>
#include <srv/svc_monitor.h>

#define GPU_MMIO_VADDR      0x30000000UL
#define GPU_APERTURE_VADDR  0x40000000UL

vec_extern(pci_device_t, pci_devices);

static endpoint_t *gpu_ep = NULL;
static pid_t gpu_spawner = PID_MAX;
static bool gpu_active = false;
static pci_device_t gpu_device;

#define GPU_MGGC0           0x50
#define GPU_BDSM            0x5C
#define GPU_GMS_SHIFT       3
#define GPU_GMS_MASK        0x1f
#define GPU_BDSM_ADDR_MASK  (0xfff << 20)

static const uint32_t gpu_gms_size[] = {
    0, 32, 64, 96, 128, 32, 48, 64, 128, 256,
    96, 160, 224, 352, 448, 480, 512
};

static void gpu_fill_mode(bootinfo_t *bi)
{
    const display_mode_t *m = display_mode_get_boot();

    bi->gpu_mode.valid = m->valid;
    bi->gpu_mode.from_edid = m->from_edid;
    bi->gpu_mode.preferred = m->preferred;
    bi->gpu_mode.pixel_clock_khz = m->pixel_clock_khz;
    bi->gpu_mode.hactive = m->hactive;
    bi->gpu_mode.hblank = m->hblank;
    bi->gpu_mode.hsync_offset = m->hsync_offset;
    bi->gpu_mode.hsync_pulse = m->hsync_pulse;
    bi->gpu_mode.vactive = m->vactive;
    bi->gpu_mode.vblank = m->vblank;
    bi->gpu_mode.vsync_offset = m->vsync_offset;
    bi->gpu_mode.vsync_pulse = m->vsync_pulse;
    bi->gpu_mode.hsync_positive = m->hsync_positive;
    bi->gpu_mode.vsync_positive = m->vsync_positive;
    bi->gpu_mode.interlaced = m->interlaced;
    bi->gpu_mode.pitch = m->pitch;
    bi->gpu_mode.refresh_hz = m->refresh_hz;
}

static void gpu_spawn_attach(process_t *tc)
{
    if (sched_get_pid() != gpu_spawner || gpu_ep == NULL)
        return;

    uint32_t id = PCI_MAKE_ID(gpu_device.bus, gpu_device.device,
                              gpu_device.func);
    pci_bar_t bar;

    pci_get_bar(&bar, id, 0);
    if (bar.u.address == NULL || bar.size == 0) {
        kloge("gpu: GTTMMADR BAR0 missing\n");
        return;
    }
    klogi("gpu: GTTMMADR phys 0x%lx size 0x%lx\n",
          (uint64_t) bar.u.address, bar.size);

    vmm_map(tc->addrspace, GPU_MMIO_VADDR, (uint64_t) bar.u.address,
            NUM_PAGES(bar.size), VMM_FLAGS_MMIO | VMM_FLAG_USER);

    uint64_t mmio_size = bar.size;

    pci_get_bar(&bar, id, 2);
    if (bar.u.address == NULL || bar.size == 0) {
        kloge("gpu: GMADR BAR2 missing\n");
        return;
    }
    klogi("gpu: GMADR phys 0x%lx size 0x%lx\n",
          (uint64_t) bar.u.address, bar.size);

    vmm_map(tc->addrspace, GPU_APERTURE_VADDR, (uint64_t) bar.u.address,
            NUM_PAGES(bar.size), VMM_FLAGS_MMIO | VMM_FLAG_USER);

    handle_t h = handle_alloc(&tc->handles, endpoint_object(gpu_ep),
                              HANDLE_RIGHT_RECV);
    if (h == HANDLE_INVALID)
        return;

    bootinfo_t *bi = kmalloc(sizeof(bootinfo_t));
    if (bi == NULL)
        return;

    memset(bi, 0, sizeof(bootinfo_t));
    bi->magic = BOOTINFO_MAGIC;
    bi->service_ep = h;
    bi->gpu_mmio_vaddr = GPU_MMIO_VADDR;
    bi->gpu_mmio_size = mmio_size;
    bi->gpu_aperture_vaddr = GPU_APERTURE_VADDR;
    bi->gpu_aperture_size = bar.size;
    bi->gpu_device_id = gpu_device.device_id;
    bi->gpu_display_ready = gfx_early_active();

    uint16_t ggc = pci_inw(id, GPU_MGGC0);
    uint32_t bdsm = pci_ind(id, GPU_BDSM);
    uint32_t gms = (ggc >> GPU_GMS_SHIFT) & GPU_GMS_MASK;

    if (gms < sizeof(gpu_gms_size) / sizeof(gpu_gms_size[0]))
        bi->gpu_stolen_size = (uint64_t)gpu_gms_size[gms] * 1024 * 1024;
    bi->gpu_stolen_base = bdsm & GPU_BDSM_ADDR_MASK;
    bi->gpu_gen9 = gfx_boot_is_gen9(gpu_device.device_id);
    bi->gpu_pte64 = gfx_boot_has_64bit_gtt(gpu_device.device_id);
    bi->gpu_gtt_offset = bi->gpu_pte64 ? 8 * 1024 * 1024 : 2 * 1024 * 1024;
    bi->gpu_gtt_size = bi->gpu_pte64 ? 8 * 1024 * 1024 : 2 * 1024 * 1024;

    fb_info_t *fb = fb_get();
    bi->gpu_boot_fb_width = fb->width;
    bi->gpu_boot_fb_height = fb->height;
    bi->gpu_boot_fb_pitch = fb->pitch;
    gpu_fill_mode(bi);

    tc->bootinfo = bi;
    klogi("gpu: boot mode %ux%u valid %d from_edid %d\n",
          (uint32_t) bi->gpu_mode.hactive, (uint32_t) bi->gpu_mode.vactive,
          (int) bi->gpu_mode.valid, (int) bi->gpu_mode.from_edid);
    klogi("gpu: granted %04x MMIO %ld MB and aperture %ld MB to pid %ld\n",
          gpu_device.device_id, (int64_t) (mmio_size / (1024 * 1024)),
          (int64_t) (bar.size / (1024 * 1024)), (int64_t) tc->pid);
}

bool gpu_server_start(void)
{
    if (gpu_active)
        return true;

    if (!gfx_boot_device(&gpu_device)) {
        klogi("gpu: no supported Intel display device\n");
        return false;
    }

    uint32_t id = PCI_MAKE_ID(gpu_device.bus, gpu_device.device,
                              gpu_device.func);

    pci_outw(id, PCI_CONFIG_COMMAND,
             pci_inw(id, PCI_CONFIG_COMMAND) | 0x6);

    gpu_ep = endpoint_create();
    if (gpu_ep == NULL)
        return false;

    const char *argv[] = { "gpu", NULL };

    gpu_spawner = sched_get_pid();
    sched_set_spawn_hook(gpu_spawn_attach);
    process_t *tc = sched_execve(DEFAULT_GPU_SVR, argv, NULL, "/");
    sched_set_spawn_hook(NULL);

    if (tc == NULL) {
        gpu_ep = NULL;
        return false;
    }

    router_register(SVC_GPU, gpu_ep, tc->pid);
    svc_monitor_set(SVC_GPU, gpu_server_start, "gpu");
    gpu_active = true;
    klogi("gpu: server started and registered as SVC_GPU\n");
    return true;
}

bool gpu_server_active(void)
{
    return gpu_active;
}

endpoint_t *gpu_srv_endpoint(void)
{
    return gpu_ep;
}

void gpu_cursor_move(int32_t dx, int32_t dy)
{
    if (!gpu_active || gpu_ep == NULL)
        return;

    ipc_msg_t m;

    memset(&m, 0, sizeof(m));
    m.tag = GPU_CURSOR_MOVE;
    m.words[0] = (uint64_t) (int64_t) dx;
    m.words[1] = (uint64_t) (int64_t) dy;
    ipc_notify(gpu_ep, &m);
}
