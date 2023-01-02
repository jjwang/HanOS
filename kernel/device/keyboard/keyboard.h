/**-----------------------------------------------------------------------------

 @file    keyboard.h
 @brief   Definition of keyboard related macros and data structures
 @details
 @verbatim

  In this module, both keyboard and mouse driver are implemented.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdint.h>

#include <sys/cpu.h>

#define KEY_COUNT                           128

#define KEYBOARD_PORT_DATA                  0x60
#define KEYBOARD_PORT_STATUS                0x64
#define KEYBOARD_PORT_CMD                   0x64

#define KEYBOARD_CMD_WRITE                  0x60
#define KEYBOARD_CMD_READ                   0x20
#define KEYBOARD_CMD_OPEN_MOUSE_INTERFACE   0xa8
#define KEYBOARD_CMD_SENDTO_MOUSE           0xd4

#define KEYBOARD_INIT_MODE                  0x47
#define KEYBOARD_MOUSE_ENABLE               0xf4

#define KEYBOARD_STATUS_INBUF_FULL          0x02
#define KEYBOARD_STATUS_OUTBUF_FULL         0x01
#define KEYBOARD_STATUS_WHICHBUF            0x20

#define WAIT_KB_WRITE() while(port_inb(KEYBOARD_PORT_STATUS) & KEYBOARD_STATUS_INBUF_FULL)
#define WAIT_KB_READ()  while(port_inb(KEYBOARD_PORT_STATUS) & KEYBOARD_STATUS_OUTBUF_FULL)

#define KEYBOARD_DISABLE_FIRST_PORT         0xAD
#define KEYBOARD_DISABLE_SECOND_PORT        0xA7

#define KEYBOARD_ENABLE_FIRST_PORT          0xAE
#define KEYBOARD_ENABLE_SECOND_PORT         0xA8

typedef struct {
    int32_t mouse_x_offset;
    int32_t mouse_y_offset;
    int32_t mouse_x;
    int32_t mouse_y;
    uint8_t mouse_cycle;
    uint8_t last_keypress;
    bool key_pressed[KEY_COUNT];
    uint8_t* ptr_to_update;
} keyboard_t;

void keyboard_init();
uint8_t keyboard_get_key();

