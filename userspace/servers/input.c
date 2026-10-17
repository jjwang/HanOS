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
#include <time.h>

#define PORT_DATA   0x60
#define PORT_CMD    0x64

/* COM1 register offsets from the base address. */
#define UART_IER    1
#define UART_LSR    5
#define UART_SCR    7

/* Software key repeat: one key on press, then repeats at a fixed period once
 * the keyboard's own typematic starts (a make code for a key already down). A
 * tap shorter than the keyboard's typematic delay never repeats, and the repeat
 * stops when those makes stop, so a lost break code cannot run a key away. */
#define REPEAT_PERIOD_MS    40
#define REPEAT_CONFIRM_MS   120
#define REPEAT_DEBOUNCE_MS  25

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

    /* Configuration byte: read the firmware value and keep its settings (in
     * particular the translation bit), then make sure both interrupts are
     * enabled and neither port disabled. Writing a fixed value could clear a
     * bit the firmware set and break the keyboard. */
    mouse_wait(1);
    outb(PORT_CMD, 0x20);
    mouse_wait(0);
    uint8_t ctr = inb(PORT_DATA);

    ctr &= (uint8_t) ~0x30;     /* clear KBDDIS and AUXDIS */
    ctr |= 0x03;                /* set KBDINT and AUXINT */

    mouse_wait(1);
    outb(PORT_CMD, 0x60);
    mouse_wait(1);
    outb(PORT_DATA, ctr);

    mouse_write(0xF6);          /* default settings */
    mouse_read();
    mouse_write(0xF4);          /* enable data reporting */
    mouse_read();
}

/* Send a keyboard command and drain its ACK. */
static void keyboard_cmd(uint8_t cmd)
{
    mouse_wait(1);
    outb(PORT_DATA, cmd);

    for (uint32_t t = 0; t < 100000; t++) {
        uint8_t s = inb(PORT_CMD);

        if (s & 0x01) {
            inb(PORT_DATA);     /* ACK (0xFA) */
            return;
        }
    }
}

/* Set a fixed typematic (30 cps after 250 ms), then start scanning. The
 * firmware often leaves scanning off, and a fixed typematic makes the repeat
 * rate independent of the keyboard's stored setting. */
static void keyboard_init(void)
{
    keyboard_cmd(0xF3);
    keyboard_cmd(0x00);
    keyboard_cmd(0xF4);
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
             || (ch >= 0x01 && ch < 0x7f))
        send_key(key_ep, ch);
}

static long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Physical key state, so a repeat make code is told apart from a real press.
 * The extended (arrow) keys use the high bit of the index. */
static bool key_down[256];
static long key_release_ms[256];        /* debounce a make after a recent break */
static int repeat_id = -1;
static uint8_t repeat_payload;
static bool repeat_arrow;
static bool repeat_started;             /* the keyboard's typematic began */
static uint8_t repeat_makes;            /* make codes seen for the down key */
static long repeat_next;
static bool repeat_typematic;   /* the keyboard is sending its own repeats */
static long repeat_make_ms;     /* time of the last make of the repeat key */

/* Deliver a new press and arm the repeat. The repeat waits for the keyboard's
 * typematic, so a tap that ends before then does not repeat. */
static void key_press(uint64_t key_ep, uint8_t idx, uint8_t payload, bool arrow)
{
    key_down[idx] = true;
    if (arrow)
        send_arrow(key_ep, payload);
    else
        send_char(key_ep, payload);

    repeat_id = idx;
    repeat_payload = payload;
    repeat_arrow = arrow;
    repeat_started = false;
    repeat_makes = 0;
    repeat_typematic = false;
    repeat_make_ms = now_ms();
}

static void key_release(uint8_t idx)
{
    key_down[idx] = false;
    key_release_ms[idx] = now_ms();
    if (repeat_id == idx) {
        repeat_id = -1;
        repeat_started = false;
        repeat_makes = 0;
    }
}

/* The keyboard sent a make for a held key. A real typematic sends makes at its
 * rate, so wait for the second before repeating: one stray make is not a hold.
 * The software repeat then caps the rate whatever the keyboard sends. */
static void repeat_begin(void)
{
    long now = now_ms();

    repeat_typematic = true;
    repeat_make_ms = now;
    if (repeat_makes < 2)
        repeat_makes++;
    if (repeat_started || repeat_makes < 2)
        return;

    repeat_started = true;
    repeat_next = now;
}

static void repeat_tick(uint64_t key_ep)
{
    if (repeat_id < 0 || !repeat_started)
        return;

    long now = now_ms();

    /* Once the keyboard's makes stop the key is up, so stop here too: a lost
     * break cannot run the repeat away. */
    if (repeat_typematic && now - repeat_make_ms > REPEAT_CONFIRM_MS) {
        repeat_id = -1;
        repeat_started = false;
        return;
    }

    if (now < repeat_next)
        return;

    if (repeat_arrow)
        send_arrow(key_ep, repeat_payload);
    else
        send_char(key_ep, repeat_payload);

    repeat_next = now + REPEAT_PERIOD_MS;
}

static uint8_t arrow_final(uint8_t sc)
{
    switch (sc) {
    case KB_ARROW_UP:
        return 'A';
    case KB_ARROW_DOWN:
        return 'B';
    case KB_ARROW_RIGHT:
        return 'C';
    case KB_ARROW_LEFT:
        return 'D';
    }
    return 0;
}

/* Decode one PS/2 byte. The kernel reads the port and hands the byte over,
 * because on some machines the i8042 byte does not survive to this process. */
static void ps2_byte(uint64_t key_ep, uint8_t raw)
{
    if (raw == 0xe0) {          /* extended prefix: the next byte picks a key */
        ext = true;
        return;
    }
    if (raw == 0xe1) {          /* pause/break, ignore */
        ext = false;
        return;
    }

    uint8_t sc = raw & 0x7f;
    bool pressed = !(raw & 0x80);
    bool was_ext = ext;

    ext = false;

    if (was_ext) {
        uint8_t idx = (uint8_t) (sc | 0x80);

        if (pressed) {
            uint8_t fin = arrow_final(sc);

            if (fin != 0) {
                if (!key_down[idx]) {
                    if (now_ms() - key_release_ms[idx] >= REPEAT_DEBOUNCE_MS)
                        key_press(key_ep, idx, fin, true);
                } else if (repeat_id == idx) {
                    repeat_begin();
                }
            }
        } else {
            key_release(idx);
        }
        return;
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
        uint8_t payload = 0;

        if (ctrl && ch >= 'a' && ch <= 'z')
            payload = (uint8_t) (ch - 'a' + 1);
        else if (ctrl && ch >= 'A' && ch <= 'Z')
            payload = (uint8_t) (ch - 'A' + 1);
        else if (ch != 0)
            payload = (uint8_t) ch;

        if (payload != 0) {
            if (!key_down[sc]) {
                if (now_ms() - key_release_ms[sc] >= REPEAT_DEBOUNCE_MS)
                    key_press(key_ep, sc, payload, false);
            } else if (repeat_id == sc) {
                repeat_begin();
            }
        } else {
            key_down[sc] = true;    /* unmapped key: track state only */
        }
    } else {
        key_release(sc);
    }
}

static void ps2_drain(uint64_t key_ep)
{
    for (uint32_t n = 0; n < 32; n++) {
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

        ps2_byte(key_ep, (uint8_t) code);
    }
}

/* Drain the serial console. Bytes already arrive as ASCII, so they go straight
 * onto the key path. */
static void serial_drain(uint64_t key_ep)
{
    if (serial_base == 0)
        return;

    for (uint32_t n = 0; n < 32; n++) {
        int64_t lsr = sys_ioport_access(0, serial_base + UART_LSR, 1, 0);
        if (lsr < 0 || !(lsr & 0x01))
            break;

        int64_t data = sys_ioport_access(0, serial_base, 1, 0);
        if (data < 0)
            break;

        /* A floating bus on a machine without COM1 reads 0xff; it is not a
         * key and would otherwise keep the loop alive forever. */
        if ((uint8_t) data == 0xff)
            break;

        send_char(key_ep, (uint8_t) data);
    }
}

static void serial_init_rx(void)
{
    if (serial_base == 0)
        return;

    /* Probe the scratch register. A machine with no COM port, or one with a
     * floating RX line, would otherwise feed noise onto the key path. */
    outb(serial_base + UART_SCR, 0x5a);
    if (inb(serial_base + UART_SCR) != 0x5a) {
        serial_base = 0;
        return;
    }
    outb(serial_base + UART_SCR, 0xa5);
    if (inb(serial_base + UART_SCR) != 0xa5) {
        serial_base = 0;
        return;
    }

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
        /* Wake on an interrupt, or early enough to emit the next repeat or
         * notice that the keyboard stopped its own repeats. */
        long timeout = 1000;

        if (repeat_id >= 0) {
            long rem = repeat_next - now_ms();

            if (repeat_typematic) {
                long conf = repeat_make_ms + REPEAT_CONFIRM_MS - now_ms();

                if (conf < rem)
                    rem = conf;
            }
            timeout = (rem > 0) ? rem : 0;
        }

        sys_ipc_msg_t m;
        int32_t r = sys_ipc_recv_timeout((int64_t) bi.irq_ep, &m, timeout);

        if (r == 0 && m.tag == IRQ_NOTIFY_TAG) {
            if (m.words[1] != 0)
                ps2_byte(bi.key_ep, (uint8_t) m.words[1]);
            ps2_drain(bi.key_ep);
            serial_drain(bi.key_ep);
        }

        repeat_tick(bi.key_ep);
    }

    return 0;
}
