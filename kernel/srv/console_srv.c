/**-----------------------------------------------------------------------------

 @file    console_srv.c
 @brief   Spawn the userspace console server and forward output to it
 @details
 @verbatim

  Creates the output endpoint and framebuffer grant, spawns /bin/console and
  starts a kernel flush thread that forwards buffered kprintf output to it.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdint.h>
#include <string.h>

#include <kconfig.h>
#include <lib/kmalloc.h>
#include <lib/klog.h>
#include <lib/spinlock.h>
#include <srv/console_srv.h>
#include <ipc/ipc.h>
#include <proc/sched.h>
#include <device/display/fb.h>
#include <srv/gpu_srv.h>
#include <mm/mm.h>
#include <bootinfo.h>

static endpoint_t *console_in_ep = NULL;
static bool console_active = false;
static pid_t console_spawner = PID_MAX;
static uint64_t console_fb_size = 0;
#define CONSOLE_FB_VADDR    0x20000000UL

/* Output from kprintf() is buffered here and forwarded by a kernel thread.
 * A syscall runs with interrupts disabled, so kprintf() must never wait for
 * the userspace server: doing so would keep the CPU and starve the server. */
#define CONSOLE_RING_SIZE   8192

static char console_ring[CONSOLE_RING_SIZE];
static uint32_t console_ring_head = 0;
static uint32_t console_ring_tail = 0;
static spinlock_t console_ring_lock = { 0 };

static bool console_ring_empty(void)
{
    return console_ring_head == console_ring_tail;
}

_Noreturn static void console_flush_kthread(pid_t pid);

static void console_spawn_attach(process_t * tc)
{
    if (sched_get_pid() != console_spawner) {
        klogw("console: spawn hook ran for pid %ld, expected %ld\n",
              (int64_t)sched_get_pid(), (int64_t)console_spawner);
        return;
    }

    fb_info_t *fb = fb_get();

    if (fb->addr == NULL || console_in_ep == NULL) {
        klogw("console: spawn hook has no framebuffer or endpoint\n");
        return;
    }

    vmm_map(tc->addrspace, CONSOLE_FB_VADDR, VIRT_TO_PHYS((uint64_t) fb->addr),
            NUM_PAGES(console_fb_size),
            VMM_FLAGS_FB_WC | VMM_FLAG_USER);

    handle_t h = handle_alloc(&tc->handles, endpoint_object(console_in_ep),
                              HANDLE_RIGHT_RECV);
    if (h == HANDLE_INVALID) {
        klogw("console: spawn hook could not allocate the endpoint handle\n");
        return;
    }

    bootinfo_t *bi = kmalloc(sizeof(bootinfo_t));
    if (bi == NULL) {
        klogw("console: spawn hook could not allocate bootinfo\n");
        return;
    }

    endpoint_t *gpu = gpu_srv_endpoint();
    handle_t hg = HANDLE_INVALID;

    if (gpu != NULL)
        hg = handle_alloc(&tc->handles, endpoint_object(gpu),
                          HANDLE_RIGHT_SEND);

    memset(bi, 0, sizeof(bootinfo_t));
    bi->magic = BOOTINFO_MAGIC;
    bi->console_ep = h;
    bi->gpu_ep = (hg == HANDLE_INVALID) ? 0 : hg;
    bi->fb_vaddr = CONSOLE_FB_VADDR;
    bi->fb_width = fb->width;
    bi->fb_height = fb->height;
    bi->fb_pitch = fb->pitch;
    bi->fb_size = console_fb_size;
    bi->desktop = 0;
    bi->fgcolor = DEFAULT_FGCOLOR;
    bi->bgcolor = DEFAULT_BGCOLOR;
    tc->bootinfo = bi;

    klogi("console: attached primary fb 0x%016lx %ux%u to pid %ld\n",
          CONSOLE_FB_VADDR, fb->width, fb->height, (int64_t)tc->pid);
}

bool console_server_start(void)
{
    fb_info_t *fb = fb_get();

    if (fb->addr == NULL)
        return false;

    console_fb_size = (uint64_t) fb->pitch * fb->height;

    console_in_ep = endpoint_create();
    if (console_in_ep == NULL)
        return false;

    /* Claim the screen before the userspace server is spawned so that kernel
     * output is forwarded to it instead of being drawn by the kernel. */
    console_active = true;

    /* The kernel may have written the scan-out through the cacheable direct map
     * while the server maps it write-combining. Flush the kernel's dirty lines
     * now, before the server starts drawing, or their later write-back would
     * corrupt its first frames. */
    asm volatile ("wbinvd":::"memory");

    const char *argv[] = { "console", NULL };

    console_spawner = sched_get_pid();
    sched_set_spawn_hook(console_spawn_attach);
    process_t *tc = sched_execve(DEFAULT_CONSOLE_SVR, argv, NULL, "/");
    sched_set_spawn_hook(NULL);

    if (tc == NULL) {
        console_active = false;
        return false;
    }

    process_t *tf = sched_new("conflush", console_flush_kthread, false);
    if (tf != NULL)
        sched_add(tf);

    klogi("console: server started (fb 0x%016lx, %ldx%ld)\n",
          CONSOLE_FB_VADDR, (int64_t) fb->width, (int64_t) fb->height);
    return true;
}

bool console_server_active(void)
{
    return console_active;
}

void console_wait_idle(void)
{
    if (!console_active || console_in_ep == NULL)
        return;

    /* Wait for the kernel flush thread to move every byte out of the ring. */
    for (uint32_t i = 0; i < 2000 && !console_ring_empty(); i++)
        sched_sleep(1);

    /* Queue a barrier behind the writes. The server replies after rendering
     * the preceding bytes, so no timing guess is involved. */
    endpoint_t *reply = endpoint_create();
    if (reply == NULL)
        return;

    object_ref(endpoint_object(reply));
    kernel_object_t *objs[1] = { endpoint_object(reply) };
    uint32_t rights[1] = { HANDLE_RIGHT_SEND };
    ipc_msg_t req;
    ipc_msg_t rep;

    memset(&req, 0, sizeof(req));
    req.tag = CONSOLE_SYNC_TAG;
    if (ipc_send_objs(console_in_ep, &req, objs, rights, 1) == 0)
        ipc_recv_timeout(reply, &rep, 1000);
    object_unref(endpoint_object(reply));
}

endpoint_t *console_srv_endpoint(void)
{
    return console_in_ep;
}

bool console_write_buf(const char *buf, uint64_t len)
{
    if (!console_active || console_in_ep == NULL)
        return false;

    spinlock_acquire(&console_ring_lock);

    bool dropped = false;

    for (uint64_t i = 0; i < len; i++) {
        uint32_t next = (console_ring_head + 1) % CONSOLE_RING_SIZE;
        if (next == console_ring_tail) {
            dropped = true;     /* ring full: drop the remainder */
            break;
        }
        console_ring[console_ring_head] = buf[i];
        console_ring_head = next;
    }

    if (dropped) {
        static bool warned = false;

        if (!warned) {
            warned = true;
            klogw("console: output ring is full, the server is not draining "
                  "it\n");
        }
    }

    spinlock_release(&console_ring_lock);
    return true;
}

/* Forward buffered output to the userspace server. This runs as its own process
 * so it may yield while the server drains, unlike the kprintf() caller. */
_Noreturn static void console_flush_kthread(pid_t pid)
{
    (void) pid;

    for (;;) {
        while (!console_ring_empty()) {
            ipc_msg_t m;
            memset(&m, 0, sizeof(m));
            m.tag = CONSOLE_WRITE_TAG;

            uint64_t n = 0;
            spinlock_acquire(&console_ring_lock);
            while (n < (IPC_WORDS - 1) * 8 && !console_ring_empty()) {
                m.words[n / 8] |=
                    (uint64_t) (uint8_t) console_ring[console_ring_tail]
                    << ((n % 8) * 8);
                console_ring_tail = (console_ring_tail + 1) % CONSOLE_RING_SIZE;
                n++;
            }
            spinlock_release(&console_ring_lock);
            m.words[IPC_WORDS - 1] = n;

            uint64_t tries = 0;
            while (ipc_send(console_in_ep, &m) != 0) {
                if (++tries > 1000)
                    break;
                sched_sleep(0);
            }

            if (tries > 1000) {
                static bool warned = false;

                if (!warned) {
                    warned = true;
                    klogw("console: server is not receiving frames\n");
                }
            } else {
                static bool first = true;

                if (first) {
                    first = false;
                    klogi("console: first frame forwarded to the server\n");
                }
            }
        }

        /* Idle poll. A longer interval cuts the wakeups that a single-core host
         * turns into vCPU contention. */
        sched_sleep(10);
    }
}
