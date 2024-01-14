/**-----------------------------------------------------------------------------

 @file    klog.c
 @brief   Implementation of kernel log related functions
 @details
 @verbatim

  A kernel-level log system was implemented. As the first step, it
  mainly supports information display.

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
#include <sys/serial.h>
#include <proc/task.h>
#include <proc/sched.h>

static klog_info_t klog_info = {0};
static klog_info_t klog_cli = {0};
static lock_t klog_info_lock = {0};

static uint64_t 
    klog_clear_times    = 0, 
    klog_refresh_times  = 0,
    klog_putchar_times  = 0;

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
    klogd("KLOG: clear %d, refresh %d and putchar %d times\n", 
          klog_clear_times, klog_refresh_times, klog_putchar_times);
}

void klog_refresh(int mode)
{
    if (term_get_redraw()) {
        klog_info_t* k = ((mode == TERM_MODE_INFO) ? &klog_info : &klog_cli);

        term_clear(mode);

        /* Note that the string ends at (k->end - 1) */
        int i = k->start;
        while (true) {
            if (i >= KLOG_BUFFER_SIZE)
                i = 0;
            if (k->end >= k->start) {
                if (i >= k->end) break;
            } else {
                if (i >= k->end && i < k->start) break;
            }
            term_putch(mode, k->buff[i]);
            klog_putchar_times++;
            i++;
        }
        klog_clear_times++;
        term_set_redraw(false);
    }

    term_refresh(mode);
    klog_refresh_times++;
}

extern void screen_write(char c);

static void klog_putch(int mode, uint8_t i)
{
    klog_info_t* k = ((mode == TERM_MODE_INFO) ? &klog_info : &klog_cli);

    k->buff[k->end] = i;
    k->end++;
    if(k->end >= KLOG_BUFFER_SIZE)
        k->end = 0;;
    if (k->end == k->start)
        k->start++;
    if(k->start >= KLOG_BUFFER_SIZE)
        k->start = 0;

    term_putch(mode, i);
    klog_putchar_times++;

    if (mode == TERM_MODE_INFO) {
        screen_write(i);
        serial_write(i);
    }
}

static void klog_puts(int mode, const char* s, int width)
{
    int cnt = 0;
    for (cnt = 0; s[cnt] != '\0'; cnt++)
        klog_putch(mode, s[cnt]);
    if(width > 0) {
        for (; cnt < width; cnt++)
            klog_putch(mode, ' ');
    }
}

static void klog_puthex(int mode, uint64_t n, int width)
{
    int cnt = 0;
    for (int i = 60; i >= 0; i -= 4) {
        cnt++;
        if(width > 0 && cnt + width <= 16) continue;
        uint64_t digit = (n >> i) & 0xF;
        klog_putch(mode, (digit <= 9) ? (digit + '0') : (digit - 10 + 'A'));
    }
}

static void klog_putint(int mode, int64_t n, int width, bool zero_filling)
{
    int64_t n_val = n;
    int n_width = 1, zero_width = 0;
    unsigned long int i = 9;
    if (n < 0) n_val = -1 * n;
    while (n_val > (int64_t)i && i < UINT64_MAX) {
        n_width += 1;
        i *= 10;
        i += 9;
    }
    if(n < 0) n_width -= 1;

    if (n < 0) {
        klog_putch(mode, '-');
        n = -n;
    }

    while(zero_width + n_width < width) {
        klog_putch(mode, zero_filling ? '0' : ' ');
        zero_width++;
    }

    if (n == 0)
        klog_putch(mode, '0');

    size_t div = 1, temp = n;
    while (temp > 0) {
        temp /= 10;
        div *= 10;
    }

    while (div >= 10) {
        uint8_t digit = ((n % div) - (n % (div / 10))) / (div / 10);
        div /= 10;
        klog_putch(mode, digit + '0');
    }
}

void klog_init()
{
    lock_lock(&klog_info_lock);

    for(int i = 0; i < KLOG_BUFFER_SIZE; i++) {
        klog_info.buff[i] = 0;
        klog_cli.buff[i] = 0;
    }

    klog_info.start = 0;
    klog_info.end = 0;

    klog_cli.start = 0;
    klog_cli.end = 0;
    
    lock_release(&klog_info_lock);
}

void klog_vprintf_core(int mode, const char* s, va_list args)
{
    for (size_t i = 0; s[i] != '\0'; i++) {
        switch (s[i]) {
        case '%': {
            uint32_t arg_width = 0;
            bool zero_filling = false;
            if(s[i + 1] == '0') zero_filling = true;
            while (s[i + 1] >= '0' && s[i + 1] <= '9') {
                arg_width *= 10;
                arg_width += s[i + 1] - '0';
                ++i;
            }
            switch (s[i + 1]) {
            case '%':
                klog_putch(mode, '%');
                break;
            case 'd':
                klog_putint(mode, va_arg(args, int64_t), arg_width, zero_filling);
                break;
            case 'x':
                klog_puthex(mode, va_arg(args, uint64_t), arg_width);
                break;
            case 's':
                klog_puts(mode, va_arg(args, const char*), arg_width);
                break;
            case 'c':
                klog_putch(mode, va_arg(args, int));
                break;
            case 'b':
                klog_puts(mode, va_arg(args, int) ? "true" : "false", 0);
                break;
            }
            i++;
        } break;
        default:
            klog_putch(mode, s[i]);
        }
    }
}

void klog_vprintf_wrapper(int mode, const char* s, ...)
{
    va_list args;
    va_start(args, s); 
    klog_vprintf_core(mode, s, args);
    va_end(args);
}

void klog_vprintf(klog_level_t level, const char* s, ...)
{
    cpu_t* cpu = smp_get_current_cpu(false);

#ifndef ENABLE_KLOG_DEBUG
    if (level <= KLOG_LEVEL_DEBUG)   return;
#else
    if (level <= KLOG_LEVEL_VERBOSE) return;
#endif
    if (level < KLOG_LEVEL_UNK)    lock_lock(&klog_info_lock);

    if (level < KLOG_LEVEL_UNK) {
        uint64_t now_sec = hpet_get_nanos() / 1000000000;
        uint64_t now_ms = (hpet_get_nanos() / 1000000) % 1000;

        time_t boot_time = cmos_boot_time();
        time_t now_time = now_sec + boot_time;

        tm_t now_tm = {0};
        localtime(&now_time, &now_tm);

        if (boot_time == 0) {
            cmos_rtc_t rt = cmos_read_rtc();
            now_tm.year = rt.year - 1900;
            now_tm.mon  = rt.month - 1;
            now_tm.mday = rt.day;
            now_tm.hour = rt.hours;
            now_tm.min  = rt.minutes;
            now_tm.sec  = rt.seconds;
        }

        klog_vprintf_wrapper(TERM_MODE_INFO, "%04d-%02d-%02d %02d:%02d:%02d %03d ",
                     1900 + now_tm.year, now_tm.mon + 1, now_tm.mday,
                     now_tm.hour, now_tm.min, now_tm.sec, now_ms);
        if (cpu != NULL) {
            klog_vprintf_wrapper(TERM_MODE_INFO, "%02d", cpu->cpu_id);
        } else {
            klog_vprintf_wrapper(TERM_MODE_INFO, "--");
        }

        task_t *t = sched_get_current_task();
        if (t != NULL) {
            klog_vprintf_wrapper(TERM_MODE_INFO, "-%03d ", t->tid);
        } else {
            klog_vprintf_wrapper(TERM_MODE_INFO, "---- ");
        }
    }

    switch (level) {
    case KLOG_LEVEL_VERBOSE:
        klog_vprintf_wrapper(TERM_MODE_INFO, "\e[34m[VERB] \e[0m ");
        break;
    case KLOG_LEVEL_DEBUG:
        klog_vprintf_wrapper(TERM_MODE_INFO, "\e[34m[DEBUG]\e[0m ");
        break;
    case KLOG_LEVEL_INFO:
        klog_vprintf_wrapper(TERM_MODE_INFO, "\e[32m[INFO] \e[0m ");
        break;
    case KLOG_LEVEL_WARN:
        klog_vprintf_wrapper(TERM_MODE_INFO, "\e[33m[WARN] \e[0m ");
        break;
    case KLOG_LEVEL_ERROR:
        klog_vprintf_wrapper(TERM_MODE_INFO, "\e[31m[ERROR]\e[0m ");
        break;
    case KLOG_LEVEL_UNK:
        break;
    }

    va_list args;
    va_start(args, s); 
    klog_vprintf_core(TERM_MODE_INFO, s, args);
    va_end(args);

    klog_refresh(TERM_MODE_INFO);
    if (level < KLOG_LEVEL_UNK) lock_release(&klog_info_lock);
}

void kprintf(const char* s, ...)
{
    lock_lock(&klog_info_lock);

    va_list args;
    va_start(args, s);
    klog_vprintf_core(TERM_MODE_CLI, s, args);
    va_end(args);

    klog_refresh(TERM_MODE_CLI);
    lock_release(&klog_info_lock);
}

