/**-----------------------------------------------------------------------------

 @file    klog.c
 @brief   Implementation of kernel log related functions
 @details
 @verbatim

  This file provides the implementation of the kernel-level log system for the
  HanOS kernel. It supports logging messages at various levels (verbose, debug,
  info, warn, error), handling log buffers, and displaying log output on the 
  terminal.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <kconfig.h>

#include <base/klog.h>
#include <base/time.h>
#include <device/display/term.h>
#include <sys/hpet.h>
#include <sys/cmos.h>
#include <sys/smp.h>
#include <sys/serial.h>
#include <proc/task.h>
#include <proc/sched.h>
#include <libc/printf.h>

static klog_info_t klog_info = { 0 };
static klog_info_t klog_cli = { 0 };

lock_t klog_info_lock = lock_new();

static lock_t klog_cli_lock = lock_new();

static uint64_t klog_clear_times = 0, klog_refresh_times = 0;

void klog_debug(void)
{
    klogd("KLOG: clear %ld, refresh %ld times\n", klog_clear_times,
          klog_refresh_times);
}

static void klog_putch(klog_info_t * k, uint8_t i)
{
    k->buff[k->end] = i;
    k->end++;
    if (k->end >= KLOG_BUFFER_SIZE)
        k->end = 0;
    if (k->end == k->start)
        k->start++;
    if (k->start >= KLOG_BUFFER_SIZE)
        k->start = 0;
}

static void klog_puts_buf(klog_info_t * k, const char *s)
{
    for (; *s != '\0'; s++)
        klog_putch(k, (uint8_t) *s);
}

void klog_vprintf_wrapper(klog_info_t * k, const char *s, ...)
{
    char buf[512];
    va_list args;
    va_start(args, s);
    vsnprintf(buf, sizeof(buf), s, args);
    va_end(args);
    klog_puts_buf(k, buf);
}

void klog_init()
{
    lock_lock(&klog_info_lock);

    klog_info.start = 0;
    klog_info.end = 0;

    lock_release(&klog_info_lock);

    lock_lock(&klog_cli_lock);

    klog_cli.start = 0;
    klog_cli.end = 0;

    lock_release(&klog_cli_lock);
}

void klog_vprintf(klog_level_t level, const char *s, ...)
{
    klog_info_t logout;         /* Fake output to speed up */
    logout.start = 0;
    logout.end = 0;
    logout.term = NULL;

#if !ENABLE_KLOG_DEBUG
    if (level <= KLOG_LEVEL_DEBUG)
        return;
#else
    if (level <= KLOG_LEVEL_VERBOSE)
        return;
#endif

    if (level < KLOG_LEVEL_UNK) {
        uint64_t now_sec = hpet_get_nanos() / 1000000000;
        uint64_t now_ms = (hpet_get_nanos() / 1000000) % 1000;

        time_t boot_time = cmos_boot_time();
        time_t now_time = now_sec + boot_time;

        tm_t now_tm = { 0 };
        localtime(&now_time, &now_tm);

        if (boot_time == 0) {
            cmos_rtc_t rt = cmos_read_rtc();
            now_tm.year = rt.year - 1900;
            now_tm.mon = rt.month - 1;
            now_tm.mday = rt.day;
            now_tm.hour = rt.hours;
            now_tm.min = rt.minutes;
            now_tm.sec = rt.seconds;
        }

        uint16_t cpu_id = smp_get_current_cpu_id();
        klog_vprintf_wrapper(&logout,
                             "%04d-%02d-%02d %02d:%02d:%02d %03d %02d ",
                             1900 + now_tm.year, now_tm.mon + 1,
                             now_tm.mday, now_tm.hour, now_tm.min,
                             now_tm.sec, now_ms, cpu_id);
    }

    switch (level) {
    case KLOG_LEVEL_VERBOSE:
        klog_puts_buf(&logout, "\e[34m[VERB] \e[0m ");
        break;
    case KLOG_LEVEL_DEBUG:
        klog_puts_buf(&logout, "\e[34m[DEBUG]\e[0m ");
        break;
    case KLOG_LEVEL_INFO:
        klog_puts_buf(&logout, "\e[32m[INFO] \e[0m ");
        break;
    case KLOG_LEVEL_WARN:
        klog_puts_buf(&logout, "\e[33m[WARN] \e[0m ");
        break;
    case KLOG_LEVEL_ERROR:
        klog_puts_buf(&logout, "\e[31m[ERROR]\e[0m ");
        break;
    case KLOG_LEVEL_UNK:
        break;
    }

    char buf[512];
    va_list args;
    va_start(args, s);
    vsnprintf(buf, sizeof(buf), s, args);
    va_end(args);
    klog_puts_buf(&logout, buf);

    lock_lock(&klog_info_lock);

    for (uint64_t i = logout.start; i < logout.end;) {
        klog_info.buff[klog_info.end] = logout.buff[i];
        klog_info.end++;

        if (klog_info.end >= KLOG_BUFFER_SIZE)
            klog_info.end = 0;
        if (klog_info.end == klog_info.start)
            klog_info.start++;
        if (klog_info.start >= KLOG_BUFFER_SIZE)
            klog_info.start = 0;

        serial_write(logout.buff[i]);

        i++;
        if (i >= KLOG_BUFFER_SIZE)
            i = 0;
    }

    lock_release(&klog_info_lock);

    klog_refresh_times++;
}

void kprintf(const char *s, ...)
{
    klog_info_t logout;         /* Fake output to speed up */
    logout.start = 0;
    logout.end = 0;
    logout.term = NULL;

    char buf[512];
    va_list args;
    va_start(args, s);
    vsnprintf(buf, sizeof(buf), s, args);
    va_end(args);
    klog_puts_buf(&logout, buf);

    lock_lock(&klog_cli_lock);

    for (uint64_t i = logout.start; i < logout.end;) {
        klog_cli.buff[klog_info.end] = logout.buff[i];
        klog_cli.end++;

        if (klog_cli.end >= KLOG_BUFFER_SIZE)
            klog_cli.end = 0;
        if (klog_cli.end == klog_cli.start)
            klog_cli.start++;
        if (klog_cli.start >= KLOG_BUFFER_SIZE)
            klog_cli.start = 0;

        term_putch(logout.buff[i]);

        i++;
        if (i >= KLOG_BUFFER_SIZE)
            i = 0;
    }

    lock_release(&klog_cli_lock);

    term_refresh();
    klog_refresh_times++;
}
