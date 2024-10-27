#include <sys/cpu.h>
#include <sys/serial.h>
#include <libc/string.h>

#define BAUD_RATE       38400

static bool serial_initialized = false;

void serial_init()
{
    port_outb(SERIAL_PORT + 3, 0x00);
    port_outb(SERIAL_PORT + 1, 0x00); /* Disable all interrupts */
    port_outb(SERIAL_PORT + 3, 0x80); /* Enable DLAB (set baud rate divisor) */

    uint16_t divisor = (uint16_t)(115200 / BAUD_RATE);
    port_outb(SERIAL_PORT + 0, divisor & 0xFF);         /* Divisor 3(lo byte) */
    port_outb(SERIAL_PORT + 1, (divisor >> 8) & 0xff);  /*          (hi byte) */

    port_outb(SERIAL_PORT + 1, 0x00);
    port_outb(SERIAL_PORT + 3, 0x03); /* 8 bits, no parity, one stop bit */
    port_outb(SERIAL_PORT + 2, 0xC7); /* Enable FIFO, clear them, with 14-byte
                                       * threshold
                                       */
    port_outb(SERIAL_PORT + 4, 0x0B); /* IRQs enabled, RTS/DSR set */

    serial_initialized = true;
}

void serial_write(char a)
{
    if (!serial_initialized) {
        serial_init();
    }

    while ((port_inb(SERIAL_PORT + 5) & 0x20) == 0);

    port_outb(SERIAL_PORT, a);
}

void serial_puts(char *s)
{
    uint64_t len = strlen(s);
    for (uint64_t i = 0; i < len; i++) {
        serial_write(s[i]);
    }
}

