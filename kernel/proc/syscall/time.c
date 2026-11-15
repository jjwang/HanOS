/**-----------------------------------------------------------------------------

 @file    time.c
 @brief   Clock, sleep and uname system calls

 **-----------------------------------------------------------------------------
 */
#include <stdint.h>
#include <string.h>
#include <errno.h>

#include <arch/x64/cpu.h>
#include <arch/x64/smp.h>
#include <arch/x64/hpet.h>
#include <arch/x64/cmos.h>
#include <lib/kmalloc.h>
#include <mm/memobj.h>
#include <mm/ipc_buf.h>
#include <mm/uaccess.h>
#include <router/router.h>
#include <protocol.h>
#include <misc.h>
#include <proc/process.h>
#include <proc/sched.h>
#include <proc/syscall.h>
#include <fs/vfs.h>
#include <proc/syscall/syscall_internal.h>

/* Copy a filled memory object back into a kernel buffer. */
static int32_t clock_now(int64_t which, vfs_timespec_t * out)
{
    vfs_timespec_t ts = { 0 };

    uint64_t now_sec = hpet_get_nanos() / 1000000000;
    uint64_t now_ns = hpet_get_nanos();

    time_t boot_time = cmos_boot_time();

    switch (which) {
    case CLOCK_REALTIME:
    case CLOCK_REALTIME_COARSE:
        ts = (vfs_timespec_t) {
        .tv_sec = now_sec + boot_time,.tv_nsec =
                now_ns + boot_time * 1000000000};
        break;
    case CLOCK_BOOTTIME:
    case CLOCK_MONOTONIC:
    case CLOCK_MONOTONIC_RAW:
    case CLOCK_MONOTONIC_COARSE:
        ts = (vfs_timespec_t) {
        .tv_sec = now_sec,.tv_nsec = now_ns};
        break;
    case CLOCK_PROCESS_CPUTIME_ID:
    case CLOCK_THREAD_CPUTIME_ID:
        ts = (vfs_timespec_t) {
        .tv_sec = 0,.tv_nsec = 0};
        break;
    default:
        return -1;
    }

    *out = ts;
    return 0;
}

int32_t k_getclock(void *_, int64_t which, vfs_timespec_t * out)
{
    (void) _;

    cpu_set_errno(0);

    if (out == NULL) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    if (router_lookup(SVC_MISC) == NULL) {
        cpu_set_errno(ENOSYS);
        return -1;
    }

    ipc_msg_t req;
    ipc_msg_t rep;

    memset(&req, 0, sizeof(req));
    req.tag = MISC_CLOCK_GETTIME;
    req.words[0] = (uint64_t) which;

    if (!router_forward(SVC_MISC, &req, &rep)) {
        cpu_set_errno(ENOSYS);
        return -1;
    }

    int64_t r = (int64_t) rep.words[0];

    if (r < 0) {
        cpu_set_errno((int32_t) (-r));
        return -1;
    }

    vfs_timespec_t ts = {
        .tv_sec = (int64_t) rep.words[1],
        .tv_nsec = (int64_t) rep.words[2],
    };

    if (copy_to_user(out, &ts, sizeof(ts)) != 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    return 0;
}

int64_t k_uname(void *buf)
{
    process_t *t = sched_get_current_process();

    cpu_set_errno(0);

    if (t == NULL || buf == NULL) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    if (router_lookup(SVC_MISC) == NULL) {
        cpu_set_errno(ENOSYS);
        return -1;
    }

    handle_t h;
    memobj_t *mo = memobj_alloc_handle(sizeof(misc_utsname_t), &h);

    if (mo == NULL) {
        cpu_set_errno(ENOMEM);
        return -1;
    }

    ipc_msg_t req;
    ipc_msg_t rep;

    memset(&req, 0, sizeof(req));
    req.tag = MISC_UNAME;
    req.xfer[0] = h;
    req.xfer_count = 1;

    bool ok = router_forward(SVC_MISC, &req, &rep)
        && (int64_t) rep.words[0] >= 0;
    misc_utsname_t u;

    if (ok)
        memobj_copy_out(mo, &u, sizeof(u), 0);

    memobj_unref(mo);

    if (!ok) {
        cpu_set_errno(ENOSYS);
        return -1;
    }

    if (copy_to_user(buf, &u, sizeof(u)) != 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    return 0;
}

int64_t k_nanosleep(vfs_timespec_t * req, vfs_timespec_t * rem)
{
    vfs_timespec_t kreq = { 0 };

    cpu_set_errno(0);

    if (rem != NULL) {
        vfs_timespec_t zero = { 0, 0 };

        copy_to_user(rem, &zero, sizeof(zero));
    }

    if (req == NULL || copy_from_user(&kreq, req, sizeof(kreq)) != 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    if (kreq.tv_sec < 0 || kreq.tv_nsec < 0 || kreq.tv_nsec >= 1000000000LL) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    int64_t ms = kreq.tv_sec * 1000 + (kreq.tv_nsec + 999999) / 1000000;

    sched_sleep(ms);
    return 0;
}

int64_t k_clock_nanosleep(int32_t clockid, int32_t flags,
                          vfs_timespec_t * req, vfs_timespec_t * rem)
{
    vfs_timespec_t kreq = { 0, 0 };

    (void)rem;
    cpu_set_errno(0);

    if (req == NULL || copy_from_user(&kreq, req, sizeof(kreq)) != 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    if (kreq.tv_nsec < 0 || kreq.tv_nsec >= 1000000000LL) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    if (flags & 1) {            /* TIMER_ABSTIME */
        vfs_timespec_t now = { 0, 0 };
        int64_t ns;

        clock_now(clockid, &now);
        ns = (kreq.tv_sec - now.tv_sec) * 1000000000LL
            + (kreq.tv_nsec - now.tv_nsec);
        if (ns <= 0)
            return 0;
        kreq.tv_sec = ns / 1000000000LL;
        kreq.tv_nsec = ns % 1000000000LL;
    } else if (kreq.tv_sec < 0) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    sched_sleep(kreq.tv_sec * 1000 + (kreq.tv_nsec + 999999) / 1000000);
    return 0;
}
