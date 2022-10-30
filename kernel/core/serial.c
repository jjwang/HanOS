#include <core/cpu.h>
#include <core/serial.h>

int serial_init()
{
    port_outb(SERIAL_PORT + 1, 0x00); /* Disable all interrupts */
    port_outb(SERIAL_PORT + 3, 0x80); /* Enable DLAB (set baud rate divisor) */
    port_outb(SERIAL_PORT + 0, 0x03); /* Divisor 3 (lo byte) 38400 baud*/
    port_outb(SERIAL_PORT + 1, 0x00); /*           (hi byte) */
    port_outb(SERIAL_PORT + 3, 0x03); /* 8 bits, no parity, one stop bit */
    port_outb(SERIAL_PORT + 2, 0xC7); /* Enable FIFO, clear them, with 14-byte
                                       * threshold
                                       */
    port_outb(SERIAL_PORT + 4, 0x0B); /* IRQs enabled, RTS/DSR set */
    port_outb(SERIAL_PORT + 4, 0x1E); /* Set in loopback mode, test the serial
                                       * chip
                                       */
    port_outb(SERIAL_PORT + 0, 0xAE); /* Test serial chip (send byte 0xAE and
                                       * check if serial returns same byte)
                                       */

    /* Check if serial is faulty (i.e: not same byte as sent) */
    if (port_inb(SERIAL_PORT + 0) != 0xAE) {
        return 1;
    }

    /*
     * If serial is not faulty set it in normal operation mode
     * (not-loopback with IRQs enabled and OUT#1 and OUT#2 bits enabled)
     */
    port_outb(SERIAL_PORT + 4, 0x0F);

    return 0;
}

void serial_write(char a)
{
    while ((port_inb(SERIAL_PORT + 5) & 0x20) == 0) {
    }
    port_outb(SERIAL_PORT, a);
}

