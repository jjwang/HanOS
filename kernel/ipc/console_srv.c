/**-----------------------------------------------------------------------------

 @file    console_srv.c
 @brief   Spawn the userspace console server and forward output to it

 **-----------------------------------------------------------------------------
 */
#include <libc/string.h>

#include <kconfig.h>
#include <base/kmalloc.h>
#include <base/klog.h>
#include <ipc/console_srv.h>
#include <ipc/ipc.h>
#include <proc/sched.h>
#include <device/display/term.h>
#include <mm/mm.h>
#include <libc/bootinfo.h>

static endpoint_t *console_in_ep = NULL;
static bool console_active = false;
static task_id_t console_spawner = TID_MAX;
static uint64_t console_fb_size = 0;

static void console_spawn_attach(task_t * tc)
{
    if (sched_get_tid() != console_spawner)
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

    const char *argv[] = { "console", NULL };

    console_spawner = sched_get_tid();
    sched_set_spawn_hook(console_spawn_attach);
    task_t *tc = sched_execve(DEFAULT_CONSOLE_SVR, argv, NULL, "/");
    sched_set_spawn_hook(NULL);

    if (tc == NULL)
        return false;

    console_active = true;
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

    uint64_t i = 0;
    while (i < len) {
        ipc_msg_t m;
        memset(&m, 0, sizeof(m));
        m.tag = CONSOLE_WRITE_TAG;

        uint64_t n = 0;
        while (n < IPC_WORDS - 1 && i < len)
            m.words[n++] = (uint8_t) buf[i++];
        m.words[IPC_WORDS - 1] = n;

        /* Spin until the server drains its queue; a bounded retry avoids
         * hanging forever if the server is gone. */
        uint64_t retries = 0;
        while (ipc_send(console_in_ep, &m) != 0 && retries++ < 10000000)
            asm volatile ("pause" ::: "memory");
    }

    return true;
}
