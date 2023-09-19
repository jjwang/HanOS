/**-----------------------------------------------------------------------------

 @file    keyboard.h
 @brief   Definition of keyboard related macros and data structures
 @details
 @verbatim

  Keyboard interrupt is triggered by IRQ1 of 8259A (master) and mouse interrupt
  is triggered by IRQ4 of 8259A (slave). Please note that slave 8259A is
  connected to master 8259A by IRQ2 on master chip.

  Mouse driver comes from SANiK.

 @endverbatim
  Ref: https://wiki.osdev.org/PS2_Keyboard

 **-----------------------------------------------------------------------------
 */
#include <libc/string.h>
#include <libc/stdio.h>

#include <device/keyboard/keyboard.h>
#include <device/keyboard/keycode.h>
#include <device/display/term.h>
#include <base/klog.h>
#include <base/lock.h>
#include <base/time.h>
#include <sys/isr_base.h>
#include <sys/cpu.h>
#include <sys/idt.h>
#include <proc/eventbus.h>

#define KB_BUFFER_SIZE    128

static volatile uint8_t key_buffer[KB_BUFFER_SIZE + 1];
static volatile uint8_t buffer_length = 0;
static volatile uint8_t read_index = 0;
static volatile uint8_t write_index = 0;

static volatile keyboard_t ps2_kb = {0};
static volatile lock_t kb_lock = lock_new();

void keyboard_set_key(bool state, uint8_t keycode)
{
    ps2_kb.key_pressed[keycode] = state;

    if (state) {
        ps2_kb.last_keypress = keycode;
    } else {
        ps2_kb.last_keypress = 0;
    }

    if (ps2_kb.ptr_to_update != NULL) {
        ps2_kb.ptr_to_update[keycode] = state;
    }
}

/*
 * One key press & release will generate 2 IRQs.
 * Press - make code, release - break code
 * break = mark + 0x80
 */
static void keyboard_callback()
{
    uint8_t key_code = port_inb(KEYBOARD_PORT_DATA);
    uint8_t scan_code = key_code & 0x7f;
    uint8_t key_state = !(key_code & 0x80);

    char ch = keyboard_get_ascii(
                  scan_code,
                  ps2_kb.key_pressed[KB_LSHIFT] | ps2_kb.key_pressed[KB_RSHIFT],
                  ps2_kb.key_pressed[KB_CAPS_LOCK]);

    keyboard_set_key(key_state, scan_code);

    while (key_state && ch != 0) {
        /* Ctrl + Shift (Left) */
        if (ps2_kb.key_pressed[KB_LSHIFT] && ps2_kb.key_pressed[KB_LCTRL])
        {
            lock_lock(&kb_lock);
            if (ch == '!' || ch == '1') {            /* Shift + '1' */
                term_switch(TERM_MODE_CLI);
                term_refresh(TERM_MODE_CLI);
            } else if (ch == '@' || ch == '2') {     /* Shift + '2' */
                term_switch(TERM_MODE_INFO);
                term_refresh(TERM_MODE_INFO);
            }
            lock_release(&kb_lock);
            break;
        }

        if (ps2_kb.key_pressed[KB_LCTRL])
        {
            if (buffer_length < KB_BUFFER_SIZE
                && (ch == 'd' || ch == 'D'))
            {
                lock_lock(&kb_lock);
                key_buffer[write_index] = EOF; 
                write_index++;
                buffer_length++;
                if (write_index == KB_BUFFER_SIZE) {
                    write_index = 0;
                }
                lock_release(&kb_lock);
                
                eb_publish(TID_NONE, EVENT_KEY_PRESSED, EOF);
                klogd("keyboard: EOF recevied!\n");
                break;
            }
        }

        if (buffer_length < KB_BUFFER_SIZE) {
            lock_lock(&kb_lock);
            key_buffer[write_index] = ch;
            write_index++;
            buffer_length++;
            if (write_index == KB_BUFFER_SIZE) {
                write_index = 0;
            }
            lock_release(&kb_lock);
        }

        eb_publish(TID_NONE, EVENT_KEY_PRESSED, ch);
        break;
    }
}

uint8_t keyboard_get_key()
{
    if (buffer_length == 0) {
        return 0;
    }
   
    uint8_t ch = key_buffer[read_index];
 
    lock_lock(&kb_lock);
    read_index++;
    buffer_length--;
    if (read_index == KB_BUFFER_SIZE) {
        read_index = 0;
    }
    lock_release(&kb_lock);
    
    return ch;
}

static void mouse_wait(uint8_t type)
{
    uint64_t _time_out = 100000;
    if (type == 0) {
        while(_time_out--) { /* Data */
            if ((port_inb(0x64) & 1) == 1) {
                return;
            }
        }
        return;
    } else {
        while (_time_out--) { /* Signal */
            if ((port_inb(0x64) & 2) == 0) {
                return;
            }
        }
        return;
    }
}

static void mouse_write(uint8_t write)
{
    /* Wait to be able to send a command */
    mouse_wait(1);

    /* Tell the mouse we are sending a command */
    port_outb(0x64, 0xd4);

    /* Wait for the final part */
    mouse_wait(1);

    /* Finally write */
    port_outb(0x60, write);
}

uint8_t mouse_read()
{
    /* Get's response from mouse */
    mouse_wait(0);
    uint8_t ack =  port_inb(0x60);
    return ack;
}

static void mouse_callback()
{
    uint8_t x = port_inb(KEYBOARD_PORT_DATA);
    (void)x;
}

void keyboard_init()
{
    isr_disable_interrupts();

    /* don't let devices send data at the wrong time and mess up initialisation */
    port_outb(KEYBOARD_PORT_CMD, KEYBOARD_DISABLE_FIRST_PORT);
    port_outb(KEYBOARD_PORT_CMD, KEYBOARD_DISABLE_SECOND_PORT); /* ignored if not supported */

    /* flush device buffer */
    while (port_inb(KEYBOARD_PORT_CMD) & KEYBOARD_STATUS_OUTBUF_FULL) {
        port_inb(KEYBOARD_PORT_DATA);
    }

    /* finilize PS/2 initialization by reenabling devices */
    port_outb(KEYBOARD_PORT_CMD, KEYBOARD_ENABLE_FIRST_PORT);
    port_outb(KEYBOARD_PORT_CMD, KEYBOARD_ENABLE_SECOND_PORT); /* ignored if unsupported? */

    uint8_t _status;

    /* Enable the auxiliary mouse device */
    mouse_wait(1);
    port_outb(0x64, 0xA8);
 
    /* Set Compaq status and enable the interrupt */
    mouse_wait(1);
    port_outb(0x64, 0x20);  
    mouse_wait(0);
    _status = (port_inb(0x60) | 2);
    mouse_wait(1);
    port_outb(0x64, 0x60);
    mouse_wait(1);
    port_outb(0x60, _status);
 
    /* Tell the mouse to use default settings */
    mouse_write(0xF6);
    mouse_read();  /* Acknowledge */

    /* Enable the mouse */
    mouse_write(0xF4);
    mouse_read();  /* Acknowledge */

    exc_register_handler(IRQ1, keyboard_callback);
    exc_register_handler(IRQ12, mouse_callback);

    /* Do not forget that IRQ2 connects to slave 8259 PIC */
    irq_clear_mask(1);
    irq_clear_mask(2);
    irq_clear_mask(12);

    isr_enable_interrupts();

    klogi("Keyboard initialization finished\n");
}

