/**-----------------------------------------------------------------------------

 @file    console_srv.c
 @brief   Spawn the userspace console server and forward output to it

 **-----------------------------------------------------------------------------
 */
#include <libc/string.h>

#include <kconfig.h>
#include <base/kmalloc.h>
#include <base/klog.h>
#include <base/spinlock.h>
#include <ipc/console_srv.h>
#include <ipc/ipc.h>
#include <proc/sched.h>
#include <device/display/term.h>
#include <mm/mm.h>
#include <libc/bootinfo.h>

static endpoint_t *console_in_ep = NULL;
static bool console_active = false;
static pid_t console_spawner = PID_MAX;
static uint64_t console_fb_size = 0;

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
    if (sched_get_pid() != console_spawner)
        return;

    fb_info_t *fb = term_get_fb();
    if (fb == NULL || fb->addr == NULL || console_in_ep == NULL)
        return;

    uint64_t vaddr = (uint64_t) fb->addr;
    uint64_t paddr = VIRT_TO_PHYS(vaddr);

    vmm_map(tc->addrspace, vaddr, paddr, NUM_PAGES(console_fb_size),
            VMM_FLAGS_FB_WC | VMM_FLAG_USER);

    handle_t h = handle_alloc(&tc->handles, endpoint_object(console_in_ep),
                              HANDLE_RIGHT_RECV);
    if (h == HANDLE_INVALID)
        return;

    uint32_t x = 0, y = 0, fg = 0, bg = 0;
    term_get_pos(&x, &y, &fg, &bg);

    bootinfo_t *bi = kmalloc(sizeof(bootinfo_t));
    if (bi == NULL)
        return;

    memset(bi, 0, sizeof(bootinfo_t));
    bi->magic = BOOTINFO_MAGIC;
    bi->console_ep = h;
    bi->fb_vaddr = vaddr;
    bi->fb_width = fb->width;
    bi->fb_height = fb->height;
    bi->fb_pitch = fb->pitch;
    bi->fb_size = console_fb_size;
    bi->cursor_x = x;
    bi->cursor_y = y;
    bi->fgcolor = fg;
    bi->bgcolor = bg;
    tc->bootinfo = bi;
}

bool console_server_start(void)
{
    fb_info_t *fb = term_get_fb();
    if (fb == NULL || fb->addr == NULL)
        return false;

    console_fb_size = (uint64_t) fb->pitch * fb->height;

    console_in_ep = endpoint_create();
    if (console_in_ep == NULL)
        return false;

    /* Claim the screen before the userspace server is spawned. From here on
     * kernel output is forwarded instead of blitted, so the server's first
     * frame cannot be overwritten by a stale kernel terminal refresh. */
    console_active = true;

    /* The kernel terminal draws the scan-out through the cacheable direct map
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
          (uint64_t) fb->addr, fb->width, fb->height);
    return true;
}

bool console_server_active(void)
{
    return console_active;
}

bool console_write_buf(const char *buf, uint64_t len)
{
    if (!console_active || console_in_ep == NULL)
        return false;

    spinlock_acquire(&console_ring_lock);

    for (uint64_t i = 0; i < len; i++) {
        uint32_t next = (console_ring_head + 1) % CONSOLE_RING_SIZE;
        if (next == console_ring_tail)
            break;              /* ring full: drop the remainder */
        console_ring[console_ring_head] = buf[i];
        console_ring_head = next;
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
            while (n < IPC_WORDS - 1 && !console_ring_empty()) {
                m.words[n++] = (uint8_t) console_ring[console_ring_tail];
                console_ring_tail = (console_ring_tail + 1) % CONSOLE_RING_SIZE;
            }
            spinlock_release(&console_ring_lock);
            m.words[IPC_WORDS - 1] = n;

            uint64_t tries = 0;
            while (ipc_send(console_in_ep, &m) != 0) {
                if (++tries > 1000)
                    break;
                sched_sleep(0);
            }
        }

        sched_sleep(1);
    }
}
