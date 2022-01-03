///-----------------------------------------------------------------------------
///
/// @file    klog.c
/// @brief   Implementation of kernel log related functions
/// @details
///   A kernel-level log system was implemented. As the first step, it
///   mainly supports information display.
/// @author  JW
/// @date    Nov 20, 2021
///
///-----------------------------------------------------------------------------
#include <stdbool.h>
#include <lib/klog.h>
#include <lib/time.h>
#include <device/display/term.h>
#include <core/hpet.h>
#include <core/cmos.h>

static klog_info_t klog_info = {0};

static void klog_refresh()
{
    term_clear();

    int i = klog_info.start;
    while(true) {
        term_putch(klog_info.buff[i]);
        i++;
        if(i >= KLOG_BUFFER_SIZE)
            i = 0;
        if(klog_info.start < klog_info.end) {
            if(i >= klog_info.end) break;
        } else {
            if(i < klog_info.start && i >= klog_info.end) break;
        }
    }

    term_refresh();
}

static void klog_putch(uint8_t i)
{
    klog_info.buff[klog_info.end] = i;
    klog_info.end++;
    if(klog_info.end >= KLOG_BUFFER_SIZE)
        klog_info.end = 0;;
    if (klog_info.end == klog_info.start)
        klog_info.start++;
    if(klog_info.start >= KLOG_BUFFER_SIZE)
        klog_info.start = 0;
}

static void klog_puts(const char* s)
{
    for (int i = 0; s[i] != '\0'; i++)
        klog_putch(s[i]);
}

static void klog_puthex(uint64_t n, int width)
{
    int cnt = 0;
    klog_puts("0x");
    for (int i = 60; i >= 0; i -= 4) {
        cnt++;
        if(width > 0 && cnt + width <= 16) continue;
        uint64_t digit = (n >> i) & 0xF;
        klog_putch((digit <= 9) ? (digit + '0') : (digit - 10 + 'A'));
    }
}

static void klog_putint(int n, int width)
{
    int n_val = n, n_width = 1, zero_width = 0;
    unsigned long int i = 9;
    if (n < 0) n_val = -1 * n;
    while (n_val > (int)i && i < UINT32_MAX) {
        n_width += 1;
        i *= 10;
        i += 9;
    }
    if(n < 0) n_width -= 1;

    if (n < 0) {
        klog_putch('-');
        n = -n;
    }

    while(zero_width + n_width < width) {
        klog_putch('0');
        zero_width++;
    }

    if (n == 0)
        klog_putch('0');

    size_t div = 1, temp = n;
    while (temp > 0) {
        temp /= 10;
        div *= 10;
    }
    while (div >= 10) {
        uint8_t digit = ((n % div) - (n % (div / 10))) / (div / 10);
        div /= 10;
        klog_putch(digit + '0');
    }
}

void klog_init()
{
    lock_lock(&klog_info.lock); 

    for(int i = 0; i < KLOG_BUFFER_SIZE; i++) {
        klog_info.buff[i] = 0;
    }
    klog_info.start = 0;
    klog_info.end = 0;
    
    lock_release(&klog_info.lock);
}

void klog_vprintf(const char* s, va_list args)
{
    for (int i = 0; s[i] != '\0'; i++) {
        switch (s[i]) {
        case '%': {
            uint32_t arg_width = 0;
            while (s[i + 1] >= '0' && s[i+1] <= '9') {
                arg_width *= 10;
                arg_width += s[i+1] - '0';
                ++i;
            }
            switch (s[i + 1]) {
            case '%':
                klog_putch('%');
                break;
            case 'd':
                klog_putint(va_arg(args, int), arg_width);
                break;
            case 'x':
                klog_puthex(va_arg(args, uint64_t), arg_width);
                break;
            case 's':
                klog_puts(va_arg(args, const char*));
                break;
            case 'b':
                klog_puts(va_arg(args, int) ? "true" : "false");
                break;
            }
            i++;
        } break;
        default:
            klog_putch(s[i]);
        }
    }
}

void klog_iprintf(const char* s, ...)
{
    va_list args;
    va_start(args, s); 
    klog_vprintf(s, args);
    va_end(args);
}

void klog_rprintf(klog_level_t level, const char* s, ...)
{
    lock_lock(&klog_info.lock)

    if(level < KLOG_LEVEL_UNK) {
        time_t now_time = hpet_get_nanos() / 1000000000 + cmos_boot_time();
        int now_ms = (hpet_get_nanos() / 1000000) % 1000;
        tm_t now_tm = {0};
        localtime(&now_time, &now_tm);
        klog_iprintf("%04d-%02d-%02d %02d:%02d:%02d %03d ",
                     1900 + now_tm.year, 1 + now_tm.mon, now_tm.mday,
                     now_tm.hour, now_tm.min, now_tm.sec, now_ms);
    }

    switch(level) {
    case KLOG_LEVEL_VERBOSE:
        klog_iprintf("\077[15;1m[VERB] \077[0m ");
        break;
    case KLOG_LEVEL_DEBUG:
        klog_iprintf("\077[15;1m[DEBUG]\077[0m ");
        break;
    case KLOG_LEVEL_INFO:
        klog_iprintf("\077[12;1m[INFO] \077[0m ");
        break;
    case KLOG_LEVEL_WARN:
        klog_iprintf("\077[13;1m[WARN] \077[0m ");
        break;
    case KLOG_LEVEL_ERROR:
        klog_iprintf("\077[11;1m[ERROR]\077[0m ");
        break;
    case KLOG_LEVEL_UNK:
        break;
    }

    va_list args;
    va_start(args, s); 
    klog_vprintf(s, args);
    va_end(args);

    klog_refresh();

    lock_release(&klog_info.lock)
}

