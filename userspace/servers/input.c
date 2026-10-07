/**-----------------------------------------------------------------------------

 @file    input.c
 @brief   Userspace PS/2 keyboard, mouse and serial input server

 @details
 @verbatim

   The kernel grants this server the PS/2 keyboard/mouse interrupts and their
   data/status ports, plus COM1 for a serial console. On each interrupt
   notification it drains the PS/2 controller, decodes scancodes into ASCII and
   the mouse's three-byte packets into deltas, and also drains any bytes that
   arrived on COM1, then hands the keys to the kernel. The controller status
   byte says whether the pending byte came from the keyboard or the mouse. The
   tty server echoes the keys.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdbool.h>
#include <stdint.h>

#include <bootinfo.h>
#include <keycode.h>
#include <sysfunc.h>

#define PORT_DATA   0x60
#define PORT_CMD    0x64

/* COM1 register offsets from the base address. */
#define UART_IER    1
#define UART_LSR    5

static uint16_t serial_base;

static inline uint8_t inb(uint16_t port)
{
    return (uint8_t) sys_ioport_access(0, port, 1, 0);
}

static inline void outb(uint16_t port, uint8_t val)
{
    sys_ioport_access(1, port, 1, val);
}

/* type 0 waits for a byte to read, type 1 for room to write. */
static void mouse_wait(uint8_t type)
{
    for (uint32_t t = 0; t < 100000; t++) {
        uint8_t s = inb(PORT_CMD);
        if (type == 0) {
            if (s & 0x01)
                return;
        } else if (!(s & 0x02)) {
            return;
        }
    }
}

static void mouse_write(uint8_t val)
{
    mouse_wait(1);
    outb(PORT_CMD, 0xD4);
    mouse_wait(1);
    outb(PORT_DATA, val);
}

static uint8_t mouse_read(void)
{
    mouse_wait(0);
    return inb(PORT_DATA);
}

static void ps2_flush(void)
{
    while (inb(PORT_CMD) & 0x01)
        inb(PORT_DATA);
}

static void mouse_init(void)
{
    /* Disable both ports and drain any byte the firmware left in the
     * controller, so a stray scancode is not mistaken for the configuration
     * byte below. */
    outb(PORT_CMD, 0xAD);       /* disable the keyboard port */
    outb(PORT_CMD, 0xA7);       /* disable the mouse port */
    ps2_flush();

    outb(PORT_CMD, 0xAE);       /* enable the keyboard port */
    outb(PORT_CMD, 0xA8);       /* enable the mouse port */

    /* Configuration byte: keyboard IRQ (bit 0), mouse IRQ (bit 1), system flag
     * (bit 2) and translation (bit 6). Writing a fixed value avoids the racy
     * read-modify-write that could otherwise disable the keyboard. */
    mouse_wait(1);
    outb(PORT_CMD, 0x60);
    mouse_wait(1);
    outb(PORT_DATA, 0x47);

    mouse_write(0xF6);          /* default settings */
    mouse_read();
    mouse_write(0xF4);          /* enable data reporting */
    mouse_read();
}

/* Tell the keyboard to start scanning and drain its ACK. The firmware often
 * leaves scanning off, so the controller delivers no scancode until this. */
static void keyboard_init(void)
{
    mouse_wait(1);
    outb(PORT_DATA, 0xF4);      /* enable scanning */

    for (uint32_t t = 0; t < 100000; t++) {
        uint8_t s = inb(PORT_CMD);

        if (s & 0x01) {
            inb(PORT_DATA);     /* ACK (0xFA) */
            break;
        }
    }
}

static void send_key(uint64_t key_ep, uint64_t ch)
{
    sys_ipc_msg_t m = { 0 };
    m.tag = INPUT_KEY_TAG;
    m.words[0] = ch;
    sys_ipc_send((int64_t) key_ep, &m);
}

/* An arrow key arrives as the VT sequence ESC [ A/B/C/D, so a full-screen
 * program reads it the way it reads a terminal. */
static void send_arrow(uint64_t key_ep, uint8_t final)
{
    send_key(key_ep, 0x1b);
    send_key(key_ep, '[');
    send_key(key_ep, final);
}

static void send_mouse(uint64_t key_ep, int32_t dx, int32_t dy)
{
    sys_ipc_msg_t m = { 0 };
    m.tag = INPUT_MOUSE_TAG;
    m.words[0] = (uint64_t) (int64_t) dx;
    m.words[1] = (uint64_t) (int64_t) dy;
    sys_ipc_send((int64_t) key_ep, &m);
}

/* Three-byte PS/2 mouse packet: flags, X delta, Y delta. */
static uint8_t mouse_cycle;
static uint8_t mouse_flags;
static int32_t mouse_dx;

static void mouse_byte(uint64_t key_ep, uint8_t data)
{
    switch (mouse_cycle) {
    case 0:
        /* Bit 3 is always set in the first byte of a packet. */
        if (!(data & 0x08))
            return;
        mouse_flags = data;
        mouse_cycle = 1;
        break;
    case 1:
        mouse_dx = (mouse_flags & 0x10) ? (int32_t) data - 256 : (int32_t) data;
        mouse_cycle = 2;
        break;
    default:
    {
        int32_t dy = (mouse_flags & 0x20) ? (int32_t) data - 256 : (int32_t) data;
        mouse_cycle = 0;
        /* PS/2 Y grows upward; screen Y grows downward. */
        send_mouse(key_ep, mouse_dx, -dy);
        break;
    }
    }
}

static bool shift;
static bool caps;
static bool ctrl;
static bool ext;                /* previous byte was the 0xE0 prefix */

/* Map a raw character to the key the tty expects and hand it to the kernel.
 * The tty server echoes it. */
static void send_char(uint64_t key_ep, uint8_t ch)
{
    if (ch == 0x04)             /* Ctrl-D: end of file */
        send_key(key_ep, INPUT_KEY_EOF);
    else if (ch == '\r')
        send_key(key_ep, '\n');
    else if (ch == 0x7f)
        send_key(key_ep, '\b');
    else if (ch == '\n' || ch == '\b' || ch == 0x1b || ch == '\t'
             || (ch >= 0x20 && ch < 0x7f))
        send_key(key_ep, ch);
}

static void ps2_drain(uint64_t key_ep)
{
    for (;;) {
        int64_t status = sys_ioport_access(0, PORT_CMD, 1, 0);
        if (status < 0 || !(status & 0x01))
            break;

        int64_t code = sys_ioport_access(0, PORT_DATA, 1, 0);
        if (code < 0)
            break;

        if ((uint8_t) status & 0x20) {
            /* Auxiliary (mouse) data. */
            mouse_byte(key_ep, (uint8_t) code);
            continue;
        }

        uint8_t raw = (uint8_t) code;

        if (raw == 0xe0) {      /* extended prefix: the next byte picks a key */
            ext = true;
            continue;
        }
        if (raw == 0xe1) {      /* pause/break, ignore */
            ext = false;
            continue;
        }

        uint8_t sc = raw & 0x7f;
        bool pressed = !(raw & 0x80);

        if (ext) {
            ext = false;
            if (pressed) {
                switch (sc) {
                case KB_ARROW_UP:
                    send_arrow(key_ep, 'A');
                    break;
                case KB_ARROW_DOWN:
                    send_arrow(key_ep, 'B');
                    break;
                case KB_ARROW_RIGHT:
                    send_arrow(key_ep, 'C');
                    break;
                case KB_ARROW_LEFT:
                    send_arrow(key_ep, 'D');
                    break;
                }
            }
            continue;
        }

        if (sc == KB_LSHIFT || sc == KB_RSHIFT) {
            shift = pressed;
        } else if (sc == KB_CAPS_LOCK) {
            if (pressed)
                caps = !caps;
        } else if (sc == KB_LCTRL) {
            ctrl = pressed;
        } else if (pressed) {
            char ch = keyboard_get_ascii(sc, shift, caps);
            if (ctrl && (ch == 'd' || ch == 'D'))
                send_char(key_ep, 0x04);
            else if (ch != 0)
                send_char(key_ep, (uint8_t) ch);
        }
    }
}

/* Drain the serial console. Bytes already arrive as ASCII, so they go straight
 * onto the key path. */
static void serial_drain(uint64_t key_ep)
{
    if (serial_base == 0)
        return;

    for (;;) {
        int64_t lsr = sys_ioport_access(0, serial_base + UART_LSR, 1, 0);
        if (lsr < 0 || !(lsr & 0x01))
            break;

        int64_t data = sys_ioport_access(0, serial_base, 1, 0);
        if (data < 0)
            break;

        send_char(key_ep, (uint8_t) data);
    }
}

static void serial_init_rx(void)
{
    if (serial_base == 0)
        return;

    /* The kernel leaves the UART with all interrupts off; enable only the
     * received-data interrupt. */
    uint8_t ier = inb(serial_base + UART_IER);
    outb(serial_base + UART_IER, ier | 0x01);
}

int32_t main(void)
{
    bootinfo_t bi;

    while (sys_bootinfo(&bi) < 0 || bi.magic != BOOTINFO_MAGIC) {
        /* The kernel sets the bootinfo before the process is runnable, so this
         * loop normally runs at most once. */
    }

    mouse_init();
    keyboard_init();

    if (bi.io_port_count > 1)
        serial_base = (uint16_t) bi.io_ports[1].first;
    serial_init_rx();

    for (;;) {
        sys_ipc_msg_t m;
        if (sys_ipc_recv((int64_t) bi.irq_ep, &m) != 0)
            continue;
        if (m.tag != IRQ_NOTIFY_TAG)
            continue;

        ps2_drain(bi.key_ep);
        serial_drain(bi.key_ep);
    }

    return 0;
}
