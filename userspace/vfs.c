/**-----------------------------------------------------------------------------

 @file    vfs.c
 @brief   Userspace VFS/name server (scaffold)

 @details
 @verbatim

   The kernel forwards FS syscalls here through the service router. This
   scaffold keeps a small fd table and serves a single synthetic file so the
   open/read/write/close path works end to end; a real namespace over the block
   server comes next.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdbool.h>
#include <stdint.h>

#include <libc/bootinfo.h>
#include <libc/protocol.h>
#include <libc/string.h>
#include <libc/sysfunc.h>

/* Where the server maps an incoming memory object (path or data). */
#define VFS_BUF_VADDR       0x20000000
#define VFS_PATH_MAX        128
#define VFS_MAX_FDS         8

static struct {
    bool in_use;
    uint64_t off;
} fds[VFS_MAX_FDS];

static const char vfs_content[] = "hello from the userspace VFS server\n";

static int fd_alloc(void)
{
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        if (!fds[i].in_use) {
            fds[i].in_use = true;
            fds[i].off = 0;
            return i + 1;
        }
    }
    return -1;
}

static void handle(sys_ipc_msg_t * m, sys_ipc_msg_t * rep)
{
    if (m->tag == VFS_PING) {
        rep->words[0] = VFS_PONG;
        return;
    }

    if (m->tag == VFS_FACCESSAT) {
        /* The path travels in xfer[1]. Keep the namespace tiny: / and /bin/...
         * and /dev/tty exist. */
        char path[VFS_PATH_MAX];
        const char *p = path;

        path[0] = '\0';
        if (m->xfer_count >= 2) {
            int64_t memh = (int64_t) m->xfer[1];
            uint8_t *buf = (uint8_t *) VFS_BUF_VADDR;
            int i = 0;

            if (sys_mem_map(memh, VFS_BUF_VADDR, 1) == 0) {
                while (i < VFS_PATH_MAX - 1 && buf[i] != '\0') {
                    path[i] = (char) buf[i];
                    i++;
                }
                sys_mem_unmap(memh, VFS_BUF_VADDR);
            }
            path[i] = '\0';
            sys_handle_close(memh);
        } else {
            p = (const char *) &m->words[1];
        }

        bool ok = (p[0] == '/' && (p[1] == '\0'
                                   || strncmp(p, "/bin/", 5) == 0
                                   || strcmp(p, "/dev/tty") == 0));
        rep->words[0] = ok ? 0 : (uint64_t) (int64_t) -2;       /* -ENOENT */
        return;
    }

    if (m->tag == VFS_OPENAT) {
        if (m->xfer_count >= 2)
            sys_handle_close((int64_t) m->xfer[1]);     /* the path */
        int fd = fd_alloc();
        if (fd < 0) {
            rep->words[0] = (uint64_t) (int64_t) -24;   /* -EMFILE */
        } else {
            rep->words[0] = 0;
            rep->words[1] = (uint64_t) fd;
        }
        return;
    }

    if (m->tag == VFS_READ) {
        int fd = (int) m->words[0];
        uint64_t len = m->words[1];
        int64_t memh = (m->xfer_count >= 2) ? (int64_t) m->xfer[1] : 0;

        if (fd < 1 || fd > VFS_MAX_FDS || !fds[fd - 1].in_use) {
            rep->words[0] = (uint64_t) (int64_t) -9;    /* -EBADF */
        } else {
            uint64_t clen = sizeof(vfs_content) - 1;
            uint64_t off = fds[fd - 1].off;
            uint64_t n = (off < clen) ? clen - off : 0;

            if (n > len)
                n = len;

            if (n > 0 && memh != 0
                && sys_mem_map(memh, VFS_BUF_VADDR, 3) == 0) {
                uint8_t *buf = (uint8_t *) VFS_BUF_VADDR;

                for (uint64_t i = 0; i < n; i++)
                    buf[i] = (uint8_t) vfs_content[off + i];
                sys_mem_unmap(memh, VFS_BUF_VADDR);
            } else {
                n = 0;
            }

            fds[fd - 1].off += n;
            rep->words[0] = 0;
            rep->words[1] = n;
        }

        if (memh != 0)
            sys_handle_close(memh);
        return;
    }

    if (m->tag == VFS_WRITE) {
        int fd = (int) m->words[0];

        if (m->xfer_count >= 2)
            sys_handle_close((int64_t) m->xfer[1]);

        if (fd < 1 || fd > VFS_MAX_FDS || !fds[fd - 1].in_use)
            rep->words[0] = (uint64_t) (int64_t) -9;    /* -EBADF */
        else {
            rep->words[0] = 0;
            rep->words[1] = m->words[1];
        }
        return;
    }

    if (m->tag == VFS_CLOSE) {
        int fd = (int) m->words[0];

        if (fd >= 1 && fd <= VFS_MAX_FDS && fds[fd - 1].in_use) {
            fds[fd - 1].in_use = false;
            rep->words[0] = 0;
        } else {
            rep->words[0] = (uint64_t) (int64_t) -9;
        }
        return;
    }

    rep->words[0] = (uint64_t) (int64_t) -38;   /* -ENOSYS */
}

int main(void)
{
    bootinfo_t bi;

    while (sys_bootinfo(&bi) < 0 || bi.magic != BOOTINFO_MAGIC) {
        /* The kernel sets the bootinfo before the process is runnable. */
    }

    for (;;) {
        sys_ipc_msg_t m;

        if (sys_ipc_recv_timeout((int64_t) bi.service_ep, &m, 1000) != 0)
            continue;

        sys_ipc_msg_t rep;
        memset(&rep, 0, sizeof(rep));
        rep.tag = m.tag;
        handle(&m, &rep);

        int64_t reply = (int64_t) m.xfer[0];
        if (reply != 0) {
            sys_ipc_send(reply, &rep);
            sys_handle_close(reply);
        }
    }

    return 0;
}
