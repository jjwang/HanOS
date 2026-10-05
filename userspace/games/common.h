/**-----------------------------------------------------------------------------

 @file    common.h
 @brief   Shared terminal and timing runtime for the HanOS games

 @details
 @verbatim

   Games run on the raw tty. This runtime enters and leaves raw mode, reads a
   key with a timeout, exposes a monotonic millisecond clock and a small random
   source, so each game keeps only its own model and drawing.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Key codes above the byte range for the arrow keys. */
#define KEY_UP      0x100
#define KEY_DOWN    0x101
#define KEY_LEFT    0x102
#define KEY_RIGHT   0x103

void game_set_raw(bool on);

/* Query the terminal grid in rows and columns. Falls back to 25x80. */
void game_size(int *rows, int *cols);

/* Print s centred on row, one-based. */
void game_center(int row, const char *s);

/* Print s centred on the last row. */
void game_menu(const char *s);

/* Read one key, waiting up to timeout_ms. Returns -1 on timeout. */
int game_read_key(int timeout_ms);

/* Monotonic milliseconds since an arbitrary origin. */
long game_ms(void);

void game_srand(uint32_t seed);
uint32_t game_rnd(void);
