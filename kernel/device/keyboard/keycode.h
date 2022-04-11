#pragma once

#define KB_ARROW_UP     72
#define KB_BACKSPACE    14
#define KB_CAPS_LOCK    58
#define KB_ENTER        28
#define KB_LCTRL        29 /* LCTRL same with RCTRL */
#define KB_LSHIFT       42
#define KB_RSHIFT       54
#define KB_TAB          15

char keyboard_get_ascii(uint8_t scancode, bool shift, bool caps_lock);

