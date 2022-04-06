/**-----------------------------------------------------------------------------

 @file    ascii.c
 @brief   Definition of keyboard's ascii code
 @details
 @verbatim

  Because this file will not be modified very often, it is separated into one
  C file.

 @endverbatim
  Ref: https://wiki.osdev.org/PS2_Keyboard
 @author  JW
 @date    Mar 12, 2022

 **-----------------------------------------------------------------------------
 */

const char ascii_default[58] =
{
    0,
    0,    // ESC
    '1',
    '2',
    '3',
    '4',
    '5',
    '6',
    '7',
    '8',
    '9',
    '0',
    '-',
    '=',
    8,
    0,
    'q',
    'w',
    'e',
    'r',
    't',
    'y',
    'u',
    'i',
    'o',
    'p',
    '[',
    ']',
    '\n',
    10,   // Return
    'a',
    's',
    'd',
    'f',
    'g',
    'h',
    'j',
    'k',
    'l',
    ';',
    '\'',
    '`',
    0,
    '\\', // BACKSLASH
    'z',
    'x',
    'c',
    'v',
    'b',
    'n',
    'm',
    ',',
    '.',
    '/',
    0,
    '*',
    0,
    ' ',  // SPACE
};

const char ascii_shift[] =
{
    0,
    0,   // ESC
    '!',
    '@',
    '#',
    '$',
    '%',
    '^',
    '&',
    '*',
    '(',
    ')',
    '_',
    '+',
    0,
    0,
    'Q',
    'W',
    'E',
    'R',
    'T',
    'Y',
    'U',
    'I',
    'O',
    'P',
    '{',
    '}',
    0,
    10,
    'A',
    'S',
    'D',
    'F',
    'G',
    'H',
    'J',
    'K',
    'L',
    ':',
    '\"',
    '~',
    0,
    '|',
    'Z',
    'X',
    'C',
    'V',
    'B',
    'N',
    'M',
    '<',
    '>',
    '?',
    0,
    0,
    0,
    ' ',  // SPACE
};

