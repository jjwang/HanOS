/**-----------------------------------------------------------------------------

 @file    mem.c
 @brief   Userspace MM server

 @details
 @verbatim

   The allocator stays in the kernel. This server answers meminfo from the
   read-only stats primitive sys_mem_stats() and returns total and free bytes
   in the reply. The kernel prints the report.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <bootinfo.h>
#include <protocol.h>
#include <sysfunc.h>

static bootinfo_t bi;

static void handle(sys_ipc_msg_t * m, sys_ipc_msg_t * rep)
{
    if (m->tag == MM_MEMINFO) {
        mm_stats_t st;

        if (sys_mem_stats(&st) == 0) {
            rep->words[0] = 0;
            rep->words[1] = st.total;
            rep->words[2] = st.free;
        } else {
            rep->words[0] = (uint64_t) (int64_t) -5;    /* -EIO */
        }
        return;
    }

    rep->words[0] = (uint64_t) (int64_t) -38;   /* -ENOSYS */
}

int32_t main(void)
{
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
