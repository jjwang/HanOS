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
#include <stdbool.h>
#include <kconfig.h>

#include <base/klog.h>
#include <base/time.h>
#include <device/display/term.h>
#include <sys/hpet.h>
#include <sys/cmos.h>
#include <sys/smp.h>
#include <proc/task.h>
#include <proc/sched.h>

static klog_info_t klog_info = { 0 };
static klog_info_t klog_cli = { 0 };
static lock_t klog_info_lock = lock_new();
static lock_t klog_cli_lock = lock_new();

static uint64_t klog_clear_times = 0, klog_refresh_times = 0;

void klog_lock(void)
{
    lock_lock(&klog_info_lock);
}

void klog_unlock(void)
{
    lock_release(&klog_info_lock);
}

void klog_debug(void)
{
    klogd("KLOG: clear %d, refresh %d times\n", klog_clear_times,
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

static void klog_puts(klog_info_t * k, const char *s, int width)
{
    int cnt = 0;
    for (cnt = 0; s[cnt] != '\0'; cnt++)
        klog_putch(k, s[cnt]);
    if (width > 0) {
        for (; cnt < width; cnt++)
            klog_putch(k, ' ');
    }
}

static void klog_puthex(klog_info_t * k, uint64_t n, int width)
{
    int cnt = 0;
    for (int i = 60; i >= 0; i -= 4) {
        cnt++;
        if (width > 0 && cnt + width <= 16)
            continue;
        uint64_t digit = (n >> i) & 0xF;
        klog_putch(k, (digit <= 9) ? (digit + '0') : (digit - 10 + 'A'));
    }
}

static void klog_putbin(klog_info_t * k, uint64_t n, int width,
                        bool mid_blank)
{
    int cnt = 0;
    for (int i = 63; i >= 0; i--) {
        cnt++;
        if (width > 0 && cnt + width <= 64)
            continue;
        uint64_t digit = (n >> i) & 0x1;
        klog_putch(k, (digit == 0) ? '0' : '1');
        if ((i % 4 == 0) && i > 0 && mid_blank)
            klog_putch(k, ' ');
    }
    klog_putch(k, 'b');
}

static void klog_putint(klog_info_t * k, int64_t n, int width,
                        bool zero_filling)
{
    int64_t n_val = n;
    int n_width = 1, zero_width = 0;
    unsigned long int i = 9;
    if (n < 0)
        n_val = -1 * n;
    while (n_val > (int64_t) i && i < UINT64_MAX) {
        n_width += 1;
        i *= 10;
        i += 9;
    }
    if (n < 0)
        n_width -= 1;

    if (n < 0) {
        klog_putch(k, '-');
        n = -n;
    }

    while (zero_width + n_width < width) {
        klog_putch(k, zero_filling ? '0' : ' ');
        zero_width++;
    }

    if (n == 0)
        klog_putch(k, '0');

    uint64_t div = 1, temp = n;
    while (temp > 0) {
        temp /= 10;
        div *= 10;
    }

    while (div >= 10) {
        uint8_t digit = ((n % div) - (n % (div / 10))) / (div / 10);
        div /= 10;
        klog_putch(k, digit + '0');
    }
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

void klog_vprintf_core(klog_info_t * k, const char *s, va_list args)
{
    for (uint64_t i = 0; s[i] != '\0'; i++) {
        switch (s[i]) {
        case '%':{
                uint32_t arg_width = 0;
                bool zero_filling = false;
                if (s[i + 1] == '0')
                    zero_filling = true;
                while (s[i + 1] >= '0' && s[i + 1] <= '9') {
                    arg_width *= 10;
                    arg_width += s[i + 1] - '0';
                    ++i;
                }
                switch (s[i + 1]) {
                case '%':
                    klog_putch(k, '%');
                    break;
                case 'd':
                    klog_putint(k, va_arg(args, int64_t), arg_width,
                                zero_filling);
                    break;
                case 'x':
                    klog_puthex(k, va_arg(args, uint64_t), arg_width);
                    break;
                case 'b':
                    klog_putbin(k, va_arg(args, uint64_t), arg_width,
                                !zero_filling);
                    break;
                case 's':
                    klog_puts(k, va_arg(args, const char *), arg_width);
                    break;
                case 'c':
                    klog_putch(k, va_arg(args, int));
                    break;
                case 't':
                    klog_puts(k, va_arg(args, int) ? "true" : "false", 0);
                    break;
                }
                i++;
            }
            break;
        default:
            klog_putch(k, s[i]);
        }
    }
}

void klog_vprintf_wrapper(klog_info_t * k, const char *s, ...)
{
    va_list args;
    va_start(args, s);
    klog_vprintf_core(k, s, args);
    va_end(args);
}

void klog_vprintf(klog_level_t level, const char *s, ...)
{
    klog_info_t logout;         /* Fake output to speed up */
    logout.start = 0;
    logout.end = 0;
    logout.term = NULL;

    cpu_t *cpu = smp_get_current_cpu(false);

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

        klog_vprintf_wrapper(&logout,
                             "%04d-%02d-%02d %02d:%02d:%02d %03d ",
                             1900 + now_tm.year, now_tm.mon + 1,
                             now_tm.mday, now_tm.hour, now_tm.min,
                             now_tm.sec, now_ms);
        if (cpu != NULL) {
            klog_vprintf_wrapper(&logout, "%02d", cpu->cpu_id);
        } else {
            klog_vprintf_wrapper(&logout, "--");
        }

        task_t *t = sched_get_current_task();
        if (t != NULL) {
            klog_vprintf_wrapper(&logout, "-%03d ", t->tid);
        } else {
            klog_vprintf_wrapper(&logout, "---- ");
        }
    }

    switch (level) {
    case KLOG_LEVEL_VERBOSE:
        klog_vprintf_wrapper(&logout, "\e[34m[VERB] \e[0m ");
        break;
    case KLOG_LEVEL_DEBUG:
        klog_vprintf_wrapper(&logout, "\e[34m[DEBUG]\e[0m ");
        break;
    case KLOG_LEVEL_INFO:
        klog_vprintf_wrapper(&logout, "\e[32m[INFO] \e[0m ");
        break;
    case KLOG_LEVEL_WARN:
        klog_vprintf_wrapper(&logout, "\e[33m[WARN] \e[0m ");
        break;
    case KLOG_LEVEL_ERROR:
        klog_vprintf_wrapper(&logout, "\e[31m[ERROR]\e[0m ");
        break;
    case KLOG_LEVEL_UNK:
        break;
    }

    va_list args;
    va_start(args, s);
    klog_vprintf_core(&logout, s, args);
    va_end(args);

    bool smp_initialized = (smp_get_current_cpu(false) != NULL);
    uint64_t msg_len = 0;

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

        if (!smp_initialized) {
            term_putch(TERM_MODE_INFO, logout.buff[i]);
        }

        i++;
        if (i >= KLOG_BUFFER_SIZE) {
            i = 0;
        }
        msg_len++;
    }

    lock_release(&klog_info_lock);

    if (smp_initialized && msg_len > 0) {
        char *msg_buff = (char*)kmalloc(msg_len + 1);
        if (msg_buff != NULL) {
            for (uint64_t i = logout.start, k = 0; i < logout.end;) {
                msg_buff[k] = logout.buff[i];
                i++;
                k++;
                if (i >= KLOG_BUFFER_SIZE)
                    i = 0;
            }
            msg_buff[msg_len] = '\0';
            kdisplay(TERM_MODE_INFO, msg_buff, msg_len);
        }
    }

    term_refresh(TERM_MODE_INFO);
    klog_refresh_times++;
}

void kprintf(const char *s, ...)
{
    klog_info_t logout;         /* Fake output to speed up */
    logout.start = 0;
    logout.end = 0;
    logout.term = NULL;

    va_list args;
    va_start(args, s);
    klog_vprintf_core(&logout, s, args);
    va_end(args);

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

#if LAUNCHER_CLI
        term_putch(TERM_MODE_CLI, logout.buff[i]);
#endif

        i++;
        if (i >= KLOG_BUFFER_SIZE)
            i = 0;
    }

    lock_release(&klog_cli_lock);

    term_refresh(TERM_MODE_CLI);
    klog_refresh_times++;
}
