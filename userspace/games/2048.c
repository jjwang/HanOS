/**-----------------------------------------------------------------------------

 @file    2048.c
 @brief   A terminal 2048 for HanOS

 @details
 @verbatim

   Runs on the raw tty and repaints a 4x4 board with ANSI cursor addressing and
   SGR colours. The board stores the value exponent, so one tile is one small
   integer. Keys: w/a/s/d or the arrow keys to move, r restart, q quit.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdbool.h>
#include <stdint.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysfunc.h>

#include "common.h"

static command_help_t help_msg[] = {
    {"<help> 2048", "Play 2048."},
};

#define N       4
#define CELL    6
#define TAB_ROW 4
#define TAB_W   (N * (CELL + 1) + 1)

static int tab_col = 6;

static const char *BORDER = "+------+------+------+------+";

static int board[N][N];
static long score;
static bool won;

static void put_at(int row, int col, const char *s)
{
    printf("\033[%d;%dH%s", row, col, s);
}

static int value(int e)
{
    return e ? 1 << e : 0;
}

static void spawn(void)
{
    int empty[N * N][2], n = 0;

    for (int r = 0; r < N; r++)
        for (int c = 0; c < N; c++)
            if (!board[r][c]) {
                empty[n][0] = r;
                empty[n][1] = c;
                n++;
            }
    if (!n)
        return;

    int p = (int) (game_rnd() % (uint32_t) n);
    int r = empty[p][0], c = empty[p][1];

    board[r][c] = (game_rnd() % 10 == 0) ? 2 : 1;
}

/* Slide one line toward index 0. Returns the gained score. */
static long slide(int *line)
{
    int tmp[N], n = 0;

    for (int i = 0; i < N; i++)
        if (line[i])
            tmp[n++] = line[i];

    int out[N] = { 0 }, o = 0;
    long gained = 0;

    for (int i = 0; i < n;) {
        if (i + 1 < n && tmp[i] == tmp[i + 1]) {
            out[o] = tmp[i] + 1;
            gained += value(out[o]);
            i += 2;
        } else {
            out[o] = tmp[i];
            i++;
        }
        o++;
    }
    for (int i = 0; i < N; i++)
        line[i] = out[i];
    return gained;
}

static void line_pos(int dir, int index, int i, int *r, int *c)
{
    switch (dir) {
    case 0:                    /* left  */
        *r = index, *c = i;
        break;
    case 1:                    /* right */
        *r = index, *c = N - 1 - i;
        break;
    case 2:                    /* up    */
        *r = i, *c = index;
        break;
    default:                   /* down  */
        *r = N - 1 - i, *c = index;
        break;
    }
}

/* Build a line in move order, slide, write it back. */
static bool move_line(int dir, int index)
{
    int line[N], before[N];

    for (int i = 0; i < N; i++) {
        int r, c;

        line_pos(dir, index, i, &r, &c);
        line[i] = board[r][c];
    }

    memcpy(before, line, sizeof(line));
    score += slide(line);

    bool changed = memcmp(before, line, sizeof(line)) != 0;

    for (int i = 0; i < N; i++) {
        int r, c;

        line_pos(dir, index, i, &r, &c);
        board[r][c] = line[i];
        if (board[r][c] >= 11)
            won = true;
    }
    return changed;
}

static bool move(int dir)
{
    bool changed = false;

    for (int i = 0; i < N; i++)
        changed = move_line(dir, i) || changed;
    return changed;
}

static bool game_over(void)
{
    for (int r = 0; r < N; r++)
        for (int c = 0; c < N; c++) {
            if (!board[r][c])
                return false;
            if (c + 1 < N && board[r][c] == board[r][c + 1])
                return false;
            if (r + 1 < N && board[r][c] == board[r + 1][c])
                return false;
        }
    return true;
}

static int color(int e)
{
    switch (e) {
    case 0:
        return 90;
    case 1:
        return 37;
    case 2:
        return 36;
    case 3:
        return 33;
    case 4:
        return 35;
    case 5:
        return 32;
    case 6:
        return 31;
    case 7:
        return 34;
    case 8:
        return 37;
    case 9:
        return 36;
    case 10:
        return 33;
    case 11:
        return 35;
    default:
        return 31;
    }
}

static void draw(void)
{
    char line[N * 20 + 8], buf[64];

    printf("\033[?25l\033[2J");
    game_center(1, "\033[1;37m2048\033[0m");
    sprintf(buf, "score %ld", score);
    game_center(2, buf);
    if (won)
        game_center(3, "\033[1;33mYou reached 2048!\033[0m");
    else
        game_center(3, "\033[90mJoin the tiles, get to 2048.\033[0m");

    put_at(TAB_ROW, tab_col, BORDER);
    for (int r = 0; r < N; r++) {
        int o = 0;

        line[o++] = '|';
        for (int c = 0; c < N; c++) {
            int v = value(board[r][c]);
            char num[12] = "";

            if (v)
                sprintf(num, "%d", v);
            o += sprintf(line + o, "\033[%dm%*s\033[0m|", color(board[r][c]),
                         CELL, num);
        }
        line[o] = '\0';
        put_at(TAB_ROW + 1 + r * 2, tab_col, line);

        if (r + 1 < N)
            put_at(TAB_ROW + 2 + r * 2, tab_col, BORDER);
    }
    put_at(TAB_ROW + N * 2, tab_col, BORDER);

    game_menu("w/a/s/d or arrows move  r restart  q quit");
    printf("\033[?25l");
    fflush(stdout);
}

static void reset(void)
{
    memset(board, 0, sizeof(board));
    score = 0;
    won = false;
    spawn();
    spawn();
}

static void restore(void)
{
    game_set_raw(false);
}

int main(void)
{
    game_set_raw(true);
    atexit(restore);

    int rows, cols;

    game_size(&rows, &cols);
    tab_col = (cols - TAB_W) / 2 + 1;
    if (tab_col < 1)
        tab_col = 1;

    game_srand((uint32_t) game_ms());
    reset();

    bool over = false;

    for (;;) {
        draw();

        if (over || game_over()) {
            over = true;
            game_center(TAB_ROW + N * 2 + 4,
                        "\033[1;31mNo moves left.  r restart  q quit\033[0m");
            fflush(stdout);
        }

        int c = game_read_key(200);

        if (c == 'q')
            break;
        if (c == 'r') {
            reset();
            over = false;
            continue;
        }
        if (over)
            continue;

        int dir = -1;

        if (c == 'a' || c == KEY_LEFT)
            dir = 0;
        else if (c == 'd' || c == KEY_RIGHT)
            dir = 1;
        else if (c == 'w' || c == KEY_UP)
            dir = 2;
        else if (c == 's' || c == KEY_DOWN)
            dir = 3;

        if (dir >= 0 && move(dir))
            spawn();
    }

    printf("\033[2J\033[?25h\033[H");
    fflush(stdout);
    return 0;
}
