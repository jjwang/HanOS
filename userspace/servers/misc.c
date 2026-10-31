/**-----------------------------------------------------------------------------

 @file    misc.c
 @brief   Userspace misc server

 @details
 @verbatim

   Owns the clock, the entropy pool and uname. The kernel forwards the
   matching syscalls to SVC_MISC. Time comes from the HPET window the kernel
   granted at startup; the boot wall-clock is a scalar in bootinfo.

   A request carries the reply endpoint in xfer[0] and, for uname and entropy,
   a buffer memory object in xfer[1]. The server maps the object, writes the
   result and unmaps it.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdbool.h>
#include <stdint.h>

#include <bootinfo.h>
#include <misc.h>
#include <string.h>
#include <sysfunc.h>
#include <time.h>

#define MISC_BUF_ADDR       0x50000000
#define MISC_HPET_COUNTER   0xF0        /* main counter inside the HPET page */

static bootinfo_t bi;
static uint64_t rng_state = 0x9e3779b97f4a7c15ULL;

static uint64_t hpet_counter(void)
{
    if (bi.misc_hpet_vaddr == 0)
        return 0;

    return *(volatile uint64_t *) (bi.misc_hpet_vaddr + MISC_HPET_COUNTER);
}

static uint64_t now_nanos(void)
{
    uint64_t c = hpet_counter();

    if (c < bi.misc_hpet_base)
        return 0;

    return (c - bi.misc_hpet_base) * bi.misc_hpet_period;
}

static uint64_t rng_next(void)
{
    uint64_t x = rng_state;

    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    rng_state = x;
    return x;
}

static void handle_clock(sys_ipc_msg_t * m, sys_ipc_msg_t * rep)
{
    int64_t which = (int64_t) m->words[0];
    uint64_t ns = now_nanos();
    uint64_t sec = ns / 1000000000ULL;
    uint64_t nsec = ns % 1000000000ULL;

    switch (which) {
    case CLOCK_REALTIME:
    case CLOCK_REALTIME_COARSE:
        sec += bi.misc_boot_time;
        break;
    case CLOCK_MONOTONIC:
    case CLOCK_MONOTONIC_RAW:
    case CLOCK_MONOTONIC_COARSE:
    case CLOCK_BOOTTIME:
        break;
    case CLOCK_PROCESS_CPUTIME_ID:
    case CLOCK_THREAD_CPUTIME_ID:
        sec = 0;
        nsec = 0;
        break;
    default:
        rep->words[0] = (uint64_t) (int64_t) -22;       /* -EINVAL */
        return;
    }

    rep->words[0] = 0;
    rep->words[1] = sec;
    rep->words[2] = nsec;
}

static void handle_uname(sys_ipc_msg_t * m, sys_ipc_msg_t * rep)
{
    int64_t memh = (m->xfer_count >= 2) ? (int64_t) m->xfer[1] : 0;

    if (memh == 0) {
        rep->words[0] = (uint64_t) (int64_t) -14;       /* -EFAULT */
        return;
    }

    if (sys_mem_map(memh, MISC_BUF_ADDR, 3) != 0) {
        sys_handle_close(memh);
        rep->words[0] = (uint64_t) (int64_t) -14;
        return;
    }

    misc_utsname_t *u = (misc_utsname_t *) MISC_BUF_ADDR;

    memset(u, 0, sizeof(*u));
    strncpy(u->sysname, "HanOS", MISC_UTS_LEN - 1);
    strncpy(u->nodename, "hanos", MISC_UTS_LEN - 1);
    strncpy(u->release, bi.misc_version, MISC_UTS_LEN - 1);
    strncpy(u->version, "HanOS ", MISC_UTS_LEN - 1);
    strncat(u->version, bi.misc_version,
            MISC_UTS_LEN - 1 - strlen(u->version));
    strncpy(u->machine, "x86_64", MISC_UTS_LEN - 1);
    strncpy(u->domainname, "(none)", MISC_UTS_LEN - 1);

    sys_mem_unmap(memh, MISC_BUF_ADDR);
    sys_handle_close(memh);
    rep->words[0] = 0;
}

static void handle_entropy(sys_ipc_msg_t * m, sys_ipc_msg_t * rep)
{
    uint64_t len = m->words[0];
    int64_t memh = (m->xfer_count >= 2) ? (int64_t) m->xfer[1] : 0;

    if (memh == 0) {
        rep->words[0] = (uint64_t) (int64_t) -14;       /* -EFAULT */
        return;
    }

    if (len > MISC_MAX_ENTROPY) {
        sys_handle_close(memh);
        rep->words[0] = (uint64_t) (int64_t) -22;       /* -EINVAL */
        return;
    }

    if (sys_mem_map(memh, MISC_BUF_ADDR, 3) != 0) {
        sys_handle_close(memh);
        rep->words[0] = (uint64_t) (int64_t) -14;
        return;
    }

    uint8_t *p = (uint8_t *) MISC_BUF_ADDR;
    uint64_t done = 0;

    while (done < len) {
        uint64_t v = rng_next();

        for (uint32_t i = 0; i < 8 && done < len; i++, done++)
            p[done] = (uint8_t) (v >> (i * 8));
    }

    sys_mem_unmap(memh, MISC_BUF_ADDR);
    sys_handle_close(memh);
    rep->words[0] = 0;
}

static void handle(sys_ipc_msg_t * m, sys_ipc_msg_t * rep)
{
    if (m->tag == MISC_CLOCK_GETTIME) {
        handle_clock(m, rep);
        return;
    }

    if (m->tag == MISC_UNAME) {
        handle_uname(m, rep);
        return;
    }

    if (m->tag == MISC_GETENTROPY) {
        handle_entropy(m, rep);
        return;
    }

    rep->words[0] = (uint64_t) (int64_t) -38;   /* -ENOSYS */
}

int32_t main(void)
{
    while (sys_bootinfo(&bi) < 0 || bi.magic != BOOTINFO_MAGIC) {
        /* The kernel sets the bootinfo before the process is runnable. */
    }

    rng_state = hpet_counter() ^ 0x9e3779b97f4a7c15ULL;
    if (rng_state == 0)
        rng_state = 0x123456789abcdef0ULL;
    for (uint32_t i = 0; i < 4; i++)
        rng_next();

    sys_server_run((int64_t) bi.service_ep, 1000, handle);

    return 0;
}
