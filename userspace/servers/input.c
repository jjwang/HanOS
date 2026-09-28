/**-----------------------------------------------------------------------------

 @file    input.c
 @brief   Userspace PS/2 keyboard and mouse input server

 @details
 @verbatim

   The kernel grants this server the keyboard and mouse interrupt lines and the
   PS/2 data/status ports. On each interrupt notification it drains the
   controller, decodes scancodes into ASCII and the mouse's three-byte packets
   into deltas, then sends them back to the kernel. The controller status byte
   says whether the pending byte came from the keyboard or the mouse.

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

static void mouse_init(void)
{
    /* Enable the auxiliary device and its interrupt (config byte bit 1). */
    mouse_wait(1);
    outb(PORT_CMD, 0xA8);

    mouse_wait(1);
    outb(PORT_CMD, 0x20);
    uint8_t status = mouse_read() | 0x02;
    mouse_wait(1);
    outb(PORT_CMD, 0x60);
    mouse_wait(1);
    outb(PORT_DATA, status);

    mouse_write(0xF6);          /* default settings */
    mouse_read();
    mouse_write(0xF4);          /* enable data reporting */
    mouse_read();
}

static void send_key(uint64_t key_ep, uint64_t ch)
{
    sys_ipc_msg_t m = { 0 };
    m.tag = INPUT_KEY_TAG;
    m.words[0] = ch;
    sys_ipc_send((int64_t) key_ep, &m);
}

static void send_mouse(uint64_t key_ep, int dx, int dy)
{
    sys_ipc_msg_t m = { 0 };
    m.tag = INPUT_MOUSE_TAG;
    m.words[0] = (uint64_t) (int64_t) dx;
    m.words[1] = (uint64_t) (int64_t) dy;
    sys_ipc_send((int64_t) key_ep, &m);
}

static void send_console(uint64_t console_ep, uint64_t ch)
{
    sys_ipc_msg_t m = { 0 };
    m.tag = CONSOLE_WRITE_TAG;
    m.words[0] = ch;
    sys_ipc_send((int64_t) console_ep, &m);
}

/* Three-byte PS/2 mouse packet: flags, X delta, Y delta. */
static uint8_t mouse_cycle;
static uint8_t mouse_flags;
static int mouse_dx;

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
        mouse_dx = (mouse_flags & 0x10) ? (int) data - 256 : (int) data;
        mouse_cycle = 2;
        break;
    default:
    {
        int dy = (mouse_flags & 0x20) ? (int) data - 256 : (int) data;
        mouse_cycle = 0;
        /* PS/2 Y grows upward; screen Y grows downward. */
        send_mouse(key_ep, mouse_dx, -dy);
        break;
    }
    }
}

int main(void)
{
    bootinfo_t bi;

    while (sys_bootinfo(&bi) < 0 || bi.magic != BOOTINFO_MAGIC) {
        /* The kernel sets the bootinfo before the process is runnable, so this
         * loop normally runs at most once. */
    }

    mouse_init();

    bool shift = false;
    bool caps = false;
    bool ctrl = false;
    int line_len = 0;

    for (;;) {
        sys_ipc_msg_t m;
        if (sys_ipc_recv((int64_t) bi.irq_ep, &m) != 0)
            continue;
        if (m.tag != IRQ_NOTIFY_TAG)
            continue;

        for (;;) {
            int64_t status = sys_ioport_access(0, PORT_CMD, 1, 0);
            if (status < 0 || !(status & 0x01))
                break;

            int64_t code = sys_ioport_access(0, PORT_DATA, 1, 0);
            if (code < 0)
                break;

            if ((uint8_t) status & 0x20) {
                /* Auxiliary (mouse) data. */
                mouse_byte(bi.key_ep, (uint8_t) code);
                continue;
            }

            uint8_t sc = (uint8_t) code & 0x7f;
            bool pressed = !((uint8_t) code & 0x80);

            if (sc == KB_LSHIFT || sc == KB_RSHIFT) {
                shift = pressed;
            } else if (sc == KB_CAPS_LOCK) {
                if (pressed)
                    caps = !caps;
            } else if (sc == KB_LCTRL) {
                ctrl = pressed;
            } else if (pressed) {
                char ch = keyboard_get_ascii(sc, shift, caps);
                if (ctrl && (ch == 'd' || ch == 'D')) {
                    send_console(bi.console_ep, '\n');
                    send_key(bi.key_ep, INPUT_KEY_EOF);
                    line_len = 0;
                } else if (ch == '\b') {
                    /* Erase only when there is something on the line. */
                    if (line_len > 0) {
                        send_console(bi.console_ep, '\b');
                        send_key(bi.key_ep, '\b');
                        line_len--;
                    }
                } else if (ch == '\n') {
                    send_console(bi.console_ep, '\n');
                    send_key(bi.key_ep, '\n');
                    line_len = 0;
                } else if (ch != 0) {
                    send_console(bi.console_ep, (uint64_t) (uint8_t) ch);
                    send_key(bi.key_ep, (uint64_t) (uint8_t) ch);
                    line_len++;
                }
            }
        }
    }

    return 0;
}
