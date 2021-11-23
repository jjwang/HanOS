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
#include <device/term.h>

void klog_init(klog_info_t* k, term_info_t* t)
{
    for(int i = 0; i < KLOG_BUFFER_SIZE; i++) {
        k->buff[i] = 0;
    }
    k->start = 0;
    k->end = 0;

    k->term = t;
}

static void klog_putch(klog_info_t* k, uint8_t i)
{
    k->buff[k->end] = i;
    k->end++;
    if(k->end >= KLOG_BUFFER_SIZE)
        k->end = 0;;
    if (k->end == k->start)
        k->start++;
    if(k->start >= KLOG_BUFFER_SIZE)
        k->start = 0;
}

static void klog_puts(klog_info_t* k, const char* s)
{
    for (int i = 0; s[i] != '\0'; i++)
        klog_putch(k, s[i]);
}

static void klog_puthex(klog_info_t* k, uint64_t n, int width)
{
    int cnt = 0;
    klog_puts(k, "0x");
    for (int i = 60; i >= 0; i -= 4) {
        cnt++;
        if(width > 0 && cnt + width <= 16) continue;
        uint64_t digit = (n >> i) & 0xF;
        klog_putch(k, (digit <= 9) ? (digit + '0') : (digit - 10 + 'A'));
    }
}

static void klog_putint(klog_info_t* k, int n, int width)
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
        klog_putch(k, '-');
        n = -n;
    }

    while(zero_width + n_width < width) {
        klog_putch(k, '0');
        zero_width++;
    }

    if (n == 0)
        klog_putch(k, '0');

    size_t div = 1, temp = n;
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

void klog_printf(klog_info_t* k, const char* s, ...)
{
    va_list args;
    va_start(args, s);

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
                klog_putch(k, '%');
                break;

            case 'd':
                klog_putint(k, va_arg(args, int), arg_width);
                break;

            case 'x':
                klog_puthex(k, va_arg(args, uint64_t), arg_width);
                break;

            case 's':
                klog_puts(k, va_arg(args, const char*));
                break;

            case 'b':
                klog_puts(k, va_arg(args, int) ? "true" : "false");
                break;
            }
            i++;
        } break;

        default:
            klog_putch(k, s[i]);
        }
    }

    va_end(args);
}

void klog_refresh(klog_info_t* k)
{
    term_clear(k->term);

    int i = k->start;
    while(true) {
        term_putch(k->term, k->buff[i]);
        i++;
        if(i >= KLOG_BUFFER_SIZE)
            i = 0;
        if(k->start < k->end) {
            if(i >= k->end) break;
        } else {
            if(i < k->start && i >= k->end) break;
        }
    }

    term_refresh(k->term);
}

