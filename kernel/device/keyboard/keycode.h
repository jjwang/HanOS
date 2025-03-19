/**-----------------------------------------------------------------------------

 @file    keycode.h
 @brief   Keycode definitions for keyboard driver
 @details
 @verbatim

  This file contains the keycode definitions and function declarations used in
  the HanOS keyboard driver. It defines constants for common keyboard keys such
  as arrow keys, backspace, caps lock, enter, control, shift, and tab. It also
  declares a function for converting scancode inputs into ASCII characters
  considering the shift and caps lock states.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#define KB_ARROW_UP     72
#define KB_BACKSPACE    14
#define KB_CAPS_LOCK    58
#define KB_ENTER        28
#define KB_LCTRL        29      /* LCTRL same with RCTRL */
#define KB_LSHIFT       42
#define KB_RSHIFT       54
#define KB_TAB          15

char keyboard_get_ascii(uint8_t scancode, bool shift, bool caps_lock);
