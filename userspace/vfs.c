/**-----------------------------------------------------------------------------

 @file    vfs.c
 @brief   Userspace VFS/name server (scaffold)

 @details
 @verbatim

   The kernel's FS syscalls are meant to be forwarded here through the service
   router. This scaffold answers VFS_PING so the router round trip is exercised;
   the namespace, mounts and fd table come next.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdint.h>

#include <libc/bootinfo.h>
#include <libc/protocol.h>
#include <libc/string.h>
#include <libc/sysfunc.h>

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

        if (m.tag == VFS_PING) {
            rep.words[0] = VFS_PONG;
        } else if (m.tag == VFS_FACCESSAT) {
            /* Minimal namespace for the router round trip. */
            const char *path = (const char *) &m.words[1];
            int ok =
                (path[0] == '/' && (path[1] == '\0'
                                    || strncmp(path, "/bin/", 5) == 0
                                    || strcmp(path, "/dev/tty") == 0));
            rep.words[0] = ok ? 0 : (uint64_t) (int64_t) -2;    /* -ENOENT */
        } else {
            rep.words[0] = (uint64_t) (int64_t) -38;    /* -ENOSYS */
        }

        int64_t reply = (int64_t) m.xfer[0];
        if (reply != 0) {
            sys_ipc_send(reply, &rep);
            sys_handle_close(reply);
        }
    }

    return 0;
}
