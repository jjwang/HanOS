/**-----------------------------------------------------------------------------

 @file    input.c
 @brief   Userspace PS/2 keyboard input server
 @details
 @verbatim

   The kernel grants this server the keyboard interrupt line and the PS/2 data
   and status ports. On each interrupt notification it drains the controller,
   decodes scancodes into ASCII and sends the decoded keys back to the kernel,
   which relays them onto the event bus for the terminal.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdbool.h>
#include <stdint.h>

#include <libc/bootinfo.h>
#include <libc/keycode.h>
#include <libc/sysfunc.h>

static void send_key(uint64_t key_ep, uint64_t ch)
{
    sys_ipc_msg_t m = { 0 };
    m.tag = INPUT_KEY_TAG;
    m.words[0] = ch;
    sys_ipc_send((int64_t) key_ep, &m);
}

static void send_console(uint64_t console_ep, uint64_t ch)
{
    sys_ipc_msg_t m = { 0 };
    m.tag = CONSOLE_WRITE_TAG;
    m.words[0] = ch;
    sys_ipc_send((int64_t) console_ep, &m);
}

int main(void)
{
    bootinfo_t bi;

    while (sys_bootinfo(&bi) < 0 || bi.magic != BOOTINFO_MAGIC) {
        /* The kernel sets the bootinfo before the task is runnable, so this
         * loop normally runs at most once. */
    }

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
            int64_t status = sys_ioport_access(0, 0x64, 1, 0);
            if (status < 0 || !(status & 0x01))
                break;

            int64_t code = sys_ioport_access(0, 0x60, 1, 0);
            if (code < 0)
                break;

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
