#include <stdarg.h>
#include <stddef.h>

#include <libc/string.h>
#include <libc/sysfunc.h>

#define CHAR_BUFF_SIZE      128

static char digits[] = "0123456789ABCDEF";
static char charbuff[CHAR_BUFF_SIZE] = {0};

static void putc(int fd, char c)
{
    size_t len = strlen(charbuff);
    if (len < CHAR_BUFF_SIZE - 1) {
        charbuff[len] = c;
        charbuff[len + 1] = '\0';
    } else { 
        sys_write(fd, charbuff, len);
        charbuff[0] = c;
        charbuff[1] = '\0';
    }
}

static void printint(int fd, int xx, int base, int sgn)
{
    char buf[16];
    int i, neg;
    unsigned int x;

    neg = 0;
    if (sgn && xx < 0) {
        neg = 1;
        x = -xx;
    } else {
        x = xx;
    }

    i = 0;
    do {
        buf[i++] = digits[x % base];
    } while((x /= base) != 0);
    if (neg)
        buf[i++] = '-';

    while (--i >= 0)
        putc(fd, buf[i]);
}

static void printptr(int fd, uint64_t x)
{
    int i;
    putc(fd, '0');
    putc(fd, 'x');
    for (i = 0; i < (int)(sizeof(uint64_t) * 2); i++, x <<= 4)
        putc(fd, digits[x >> (sizeof(uint64_t) * 8 - 4)]);
}

/* Print to the given fd. Only understands %d, %x, %p, %s. */
void vprintf(int fd, const char *fmt, va_list ap)
{
    char *s;
    int c, i, state;

    state = 0;
    for (i = 0; fmt[i]; i++) {
        c = fmt[i] & 0xff;
        if (state == 0) {
            if (c == '%') {
                state = '%';
            } else {
                putc(fd, c);
            }
        } else if (state == '%') {
            if (c == 'd') {
                printint(fd, va_arg(ap, int), 10, 1);
            } else if (c == 'l') {
                printint(fd, va_arg(ap, uint64_t), 10, 0);
            } else if (c == 'x') {
                printint(fd, va_arg(ap, int), 16, 0);
            } else if (c == 'p') {
                printptr(fd, va_arg(ap, uint64_t));
            } else if (c == 's'){
                s = va_arg(ap, char*);
                if(s == 0)
                    s = "(null)";
                while (*s != 0) {
                    putc(fd, *s);
                    s++;
                }
            } else if (c == 'c') {
                putc(fd, va_arg(ap, unsigned int));
            } else if (c == '%') {
                putc(fd, c);
            } else {
                /* Unknown % sequence. Print it to draw attention. */
                putc(fd, '%');
                putc(fd, c);
            }
            state = 0;
        }
    }
}

void fprintf(int fd, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vprintf(fd, fmt, ap);

    sys_write(fd, charbuff, strlen(charbuff));
    charbuff[0] = '\0';
}

void printf(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vprintf(STDOUT, fmt, ap);

    sys_write(STDOUT, charbuff, strlen(charbuff));
    charbuff[0] = '\0';
}

