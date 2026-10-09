/**-----------------------------------------------------------------------------

 @file    common.c
 @brief   Shared terminal and timing runtime for the HanOS games

 @details
 @verbatim

   See common.h.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "common.h"

static struct termios saved;

void game_set_raw(bool on)
{
    if (on) {
        tcgetattr(STDIN_FILENO, &saved);
        struct termios t = saved;

        t.c_lflag &= ~(tcflag_t) (ECHO | ICANON);
        tcsetattr(STDIN_FILENO, TCSANOW, &t);
    } else {
        tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    }
}

void game_size(int *rows, int *cols)
{
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row && ws.ws_col) {
        *rows = ws.ws_row;
        *cols = ws.ws_col;
    } else {
        *rows = 25;
        *cols = 80;
    }
}

void game_center(int row, const char *s)
{
    int rows, cols;

    game_size(&rows, &cols);

    int len = (int) strlen(s);
    int col = (cols - len) / 2;

    if (col < 1)
        col = 1;
    printf("\033[%d;%dH\033[0m%s", row, col, s);
}

void game_menu(const char *s)
{
    int rows, cols;

    game_size(&rows, &cols);

    int len = (int) strlen(s);
    int col = (cols - len) / 2;

    if (col < 1)
        col = 1;

    /* Clear the row first: a shorter menu must not leave the old one behind. */
    printf("\033[%d;1H\033[2K\033[%d;%dH\033[0m%s", rows, rows, col, s);
}

static int poll_byte(int timeout_ms)
{
    struct pollfd pf = { STDIN_FILENO, POLLIN, 0 };

    if (poll(&pf, 1, timeout_ms) != 1)
        return -1;

    unsigned char c;

    if (read(STDIN_FILENO, &c, 1) != 1)
        return -1;
    return c;
}

/* Assemble an escape sequence across reads. The tty delivers the bytes of
 * ESC [ A/B/C/D as separate keys, so a fixed window would drop a sequence on a
 * slow machine. The state carries over between calls: a byte that arrives late
 * completes the sequence instead of being lost, and a stray or truncated ESC
 * hands the next real key back untouched. */
static int esc_state;           /* 0 none, 1 after ESC, 2 after ESC [ or ESC O */

int game_read_key(int timeout_ms)
{
    for (;;) {
        int c = poll_byte(timeout_ms);

        if (c < 0)
            return -1;

        if (esc_state == 0) {
            if (c == 0x1b) {
                esc_state = 1;
                continue;
            }
            return c;
        }

        if (esc_state == 1) {
            if (c == '[' || c == 'O') {
                esc_state = 2;
                continue;
            }
            esc_state = 0;
            return c;           /* the ESC stood alone: deliver the key */
        }

        esc_state = 0;
        switch (c) {
        case 'A':
            return KEY_UP;
        case 'B':
            return KEY_DOWN;
        case 'C':
            return KEY_RIGHT;
        case 'D':
            return KEY_LEFT;
        default:
            return c;
        }
    }
}

long game_ms(void)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static uint32_t rng = 0x12345678;

void game_srand(uint32_t seed)
{
    rng = seed ? seed : 0x12345678;
}

uint32_t game_rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}
