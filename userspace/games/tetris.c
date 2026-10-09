/**-----------------------------------------------------------------------------

 @file    tetris.c
 @brief   A terminal Tetris for HanOS

 @details
 @verbatim

   Runs on the raw tty: turns off echo and canonical input, hides the cursor,
   and repaints a 10x20 well with ANSI cursor addressing and SGR colours. Keys:
   a/d move, s soft drop, w rotate, space hard drop, p pause, q quit.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdbool.h>
#include <stdint.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sysfunc.h>
#include <time.h>

#include "common.h"

static command_help_t help_msg[] = {
    {"<help> tetris", "Play Tetris."},
};

#define BW          10
#define BH          20
#define WELL_ROW    3
#define WELL_W      (BW * 2 + 2)

static int well_col = 4;

/* Four rotations per piece as 4x4 bit masks (row 0 is the top). */
static const uint16_t shapes[7][4] = {
    { 0x0F00, 0x2222, 0x00F0, 0x4444 }, /* I */
    { 0x0660, 0x0660, 0x0660, 0x0660 }, /* O */
    { 0x0E40, 0x4C40, 0x4E00, 0x4640 }, /* T */
    { 0x06C0, 0x8C40, 0x06C0, 0x8C40 }, /* S */
    { 0x0C60, 0x4C80, 0x0C60, 0x4C80 }, /* Z */
    { 0x44C0, 0x8E00, 0x6440, 0x0E20 }, /* J */
    { 0x4460, 0x0E80, 0xC440, 0x2E00 }, /* L */
};

static const int colors[7] = { 36, 33, 35, 32, 31, 34, 37 };

static int board[BH][BW];
static int px, py, prot, ptype;
static long score;
static int lines;
static int level;

/* Soft drop is armed only after the key stops repeating. Locking a piece
 * disarms it, so a key still held does not accelerate the next piece; the
 * player must release and press again. */
#define SOFT_RELEASE_MS 150

static long last_drop;
static bool soft_armed = true;
static long last_down;
static bool over;

static uint32_t rng = 0x12345678;

static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static bool cell(uint16_t m, int r, int c)
{
    return (m >> ((3 - r) * 4 + (3 - c))) & 1;
}

static bool fits(int type, int rot, int x, int y)
{
    const uint16_t m = shapes[type][rot];

    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            if (!cell(m, r, c))
                continue;
            int bx = x + c, by = y + r;

            if (bx < 0 || bx >= BW || by >= BH)
                return false;
            if (by >= 0 && board[by][bx])
                return false;
        }
    return true;
}

static void spawn(void)
{
    ptype = (int) (rnd() % 7);
    prot = 0;
    px = 3;
    py = 0;
    if (!fits(ptype, prot, px, py)) {
        /* Top out: the next piece does not fit. End the game and keep the
         * board so the player sees the final position. */
        over = true;
    }
}

static void reset_game(void)
{
    memset(board, 0, sizeof(board));
    score = 0;
    lines = 0;
    level = 1;
    over = false;
    spawn();
}

/* Centre text in a space-wide band, so the game-over bar spans the whole
 * well rather than hugging the words. */
static void center_band(char *out, const char *text, int space)
{
    int n = (int) strlen(text);
    int pad = (space - n) / 2;
    int i = 0;

    if (pad < 0)
        pad = 0;
    for (; i < pad; i++)
        out[i] = ' ';
    for (int j = 0; j < n && i < space; j++)
        out[i++] = text[j];
    while (i < space)
        out[i++] = ' ';
    out[i] = '\0';
}

static void lock_piece(void)
{
    const uint16_t m = shapes[ptype][prot];

    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            if (cell(m, r, c) && py + r >= 0 && py + r < BH && px + c >= 0
                && px + c < BW)
                board[py + r][px + c] = ptype + 1;

    /* Clear full rows. */
    int cleared = 0;

    for (int r = BH - 1; r >= 0; r--) {
        bool full = true;

        for (int c = 0; c < BW; c++)
            if (!board[r][c])
                full = false;
        if (full) {
            cleared++;
            for (int rr = r; rr > 0; rr--)
                memcpy(board[rr], board[rr - 1], sizeof(board[0]));
            memset(board[0], 0, sizeof(board[0]));
            r++;
        }
    }

    if (cleared) {
        score += (cleared == 1) ? 100 : (cleared == 2) ? 300
            : (cleared == 3) ? 500 : 800;
        lines += cleared;
        level = 1 + lines / 10;
    }

    spawn();
    last_drop = game_ms();
    soft_armed = false;
}

static void draw(void)
{
    char line[BW * 12 + 24];
    char frame[16384];
    int f = 0;

    f += sprintf(frame + f,
                 "\033[?25l\033[1;%dH\033[37mTETRIS  score %ld  lines %d  level %d\033[0m",
                 well_col, score, lines, level);

    for (int r = 0; r < BH; r++) {
        int o = 0;

        o += sprintf(line + o, "\033[37m|\033[0m");
        for (int c = 0; c < BW; c++) {
            int v = board[r][c];
            int pr = r - py, pc = c - px;

            if (!over && v == 0 && pr >= 0 && pr < 4 && pc >= 0 && pc < 4
                && cell(shapes[ptype][prot], pr, pc))
                v = ptype + 1;

            if (v == 0)
                o += sprintf(line + o, "  ");
            else
                o += sprintf(line + o, "\033[%dm[]", colors[v - 1]);
        }
        o += sprintf(line + o, "\033[0m|");
        f += sprintf(frame + f, "\033[%d;%dH%s", WELL_ROW + r, well_col, line);
    }

    f += sprintf(frame + f, "\033[%d;%dH+--------------------+\033[?25l",
                 WELL_ROW + BH, well_col);

    if (over) {
        /* Reverse video reads white-on-black whatever the palette does to the
         * ANSI colours. The bar spans the full well width. */
        char b1[WELL_W + 1];
        char b2[WELL_W + 1];
        char sc[24];

        snprintf(sc, sizeof(sc), "score %ld", score);
        center_band(b1, "GAME OVER", WELL_W);
        center_band(b2, sc, WELL_W);
        f += sprintf(frame + f, "\033[%d;%dH\033[7m%s\033[0m",
                     WELL_ROW + BH / 2 - 1, well_col, b1);
        f += sprintf(frame + f, "\033[%d;%dH\033[7m%s\033[0m",
                     WELL_ROW + BH / 2 + 1, well_col, b2);
    }

    fwrite(frame, 1, (size_t) f, stdout);
    fflush(stdout);

    game_menu(over ? "GAME OVER  r restart  q quit"
                   : "arrows or a/d move  w up rotate  s down  space drop  p pause  q quit");
}

static void restore_term(void)
{
    game_set_raw(false);
}

int main(void)
{
    game_set_raw(true);
    atexit(restore_term);
    printf("\033[2J\033[?25l");

    int rows, cols;

    game_size(&rows, &cols);
    well_col = (cols - WELL_W) / 2 + 1;
    if (well_col < 1)
        well_col = 1;

    rng ^= (uint32_t) time(NULL);
    score = 0;
    lines = 0;
    level = 1;
    memset(board, 0, sizeof(board));
    spawn();

    bool paused = false;
    bool dirty = true;

    last_drop = game_ms();
    soft_armed = true;
    last_down = 0;

    for (;;) {
        long gravity = 700 - (level - 1) * 60;

        if (gravity < 120)
            gravity = 120;

        int c = game_read_key(10);

        if (c != -1) {
            if (c == 'q')
                break;
            if (over) {
                if (c == 'r') {
                    reset_game();
                    paused = false;
                    last_drop = game_ms();
                    soft_armed = true;
                    last_down = 0;
                    dirty = true;
                }
            } else if (c == 'p') {
                paused = !paused;
                dirty = true;
            } else if (!paused) {
                if ((c == 'a' || c == KEY_LEFT)
                    && fits(ptype, prot, px - 1, py)) {
                    px--;
                    dirty = true;
                } else if ((c == 'd' || c == KEY_RIGHT)
                           && fits(ptype, prot, px + 1, py)) {
                    px++;
                    dirty = true;
                } else if ((c == 'w' || c == KEY_UP)
                           && fits(ptype, (prot + 1) % 4, px, py)) {
                    prot = (prot + 1) % 4;
                    dirty = true;
                } else if (c == 's' || c == KEY_DOWN) {
                    last_down = game_ms();
                    if (soft_armed) {
                        if (fits(ptype, prot, px, py + 1))
                            py++;
                        else
                            lock_piece();
                        dirty = true;
                    }
                } else if (c == ' ') {
                    while (fits(ptype, prot, px, py + 1))
                        py++;
                    lock_piece();
                    dirty = true;
                }
            }
        }

        /* Gravity follows real elapsed time, not the loop rate: a slow repaint
         * must not slow the drop. */
        long ms = game_ms();

        if (!over && !paused && ms - last_drop >= gravity) {
            last_drop = ms;
            if (fits(ptype, prot, px, py + 1))
                py++;
            else
                lock_piece();
            dirty = true;
        }

        /* Re-arm soft drop once the key has been quiet long enough to count as
         * released. */
        if (!over && !soft_armed && ms - last_down > SOFT_RELEASE_MS)
            soft_armed = true;

        /* Repaint only on a change, so a key is read within one short poll. */
        if (dirty) {
            draw();
            dirty = false;
        }
    }

    game_set_raw(false);
    printf("\033[2J\033[?25h\033[H");
    fflush(stdout);
    return 0;
}
