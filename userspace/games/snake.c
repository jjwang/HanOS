/**-----------------------------------------------------------------------------

 @file    snake.c
 @brief   A terminal Snake for HanOS

 @details
 @verbatim

   Runs on the raw tty with a fixed-step loop. The snake is a ring buffer of
   cells, so a step rewrites the head and drops the tail in constant time. Keys:
   w/a/s/d or the arrow keys to turn, p pause, q quit.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdbool.h>
#include <stdint.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sysfunc.h>

#include "common.h"

static command_help_t help_msg[] = {
    {"<help> snake", "Play Snake."},
};

#define GW          40
#define GH          20
#define TAB_ROW     4
#define TAB_W       (GW + 2)

static int tab_col = 4;

static int sx[GW * GH], sy[GW * GH];
static int head, len;
static int fx, fy;
static int dx, dy;
static int pending_dx, pending_dy;
static long score;

static void put_at(int row, int col, const char *s)
{
    printf("\033[%d;%dH%s", row, col, s);
}

static int cell_index(int i)
{
    return (head - i + GW * GH) % (GW * GH);
}

static void place_food(void)
{
    for (;;) {
        int x = (int) (game_rnd() % GW);
        int y = (int) (game_rnd() % GH);
        bool on = false;

        for (int i = 0; i < len; i++)
            if (sx[cell_index(i)] == x && sy[cell_index(i)] == y)
                on = true;
        if (!on) {
            fx = x;
            fy = y;
            return;
        }
    }
}

static void reset(void)
{
    memset(sx, 0, sizeof(sx));
    memset(sy, 0, sizeof(sy));
    head = 0;
    len = 3;
    for (int i = 0; i < len; i++) {
        int idx = (head - i + GW * GH) % (GW * GH);

        sx[idx] = GW / 2 - i;
        sy[idx] = GH / 2;
    }
    dx = 1;
    dy = 0;
    pending_dx = 1;
    pending_dy = 0;
    score = 0;
    place_food();
}

static void draw(void)
{
    char line[GW * 14 + 16];

    printf("\033[?25l\033[2J");
    char buf[64];

    game_center(1, "\033[1;32mSNAKE\033[0m");
    sprintf(buf, "score %ld  length %d", score, len);
    game_center(2, buf);

    int o = 0;

    o += sprintf(line + o, "\033[37m+");
    for (int c = 0; c < GW; c++)
        o += sprintf(line + o, "-");
    o += sprintf(line + o, "+\033[0m");
    put_at(TAB_ROW, tab_col, line);

    for (int r = 0; r < GH; r++) {
        o = 0;
        o += sprintf(line + o, "\033[37m|\033[0m");
        for (int c = 0; c < GW; c++) {
            char ch = ' ';
            const char *col = "";

            if (c == fx && r == fy) {
                ch = '$';
                col = "\033[1;31m";
            } else {
                for (int i = 0; i < len; i++) {
                    if (sx[cell_index(i)] == c && sy[cell_index(i)] == r) {
                        ch = i == 0 ? '@' : 'o';
                        col = i == 0 ? "\033[1;32m" : "\033[32m";
                        break;
                    }
                }
            }
            if (ch == ' ')
                o += sprintf(line + o, " ");
            else
                o += sprintf(line + o, "%s%c\033[0m", col, ch);
        }
        o += sprintf(line + o, "\033[37m|\033[0m");
        put_at(TAB_ROW + 1 + r, tab_col, line);
    }

    o = 0;
    o += sprintf(line + o, "\033[37m+");
    for (int c = 0; c < GW; c++)
        o += sprintf(line + o, "-");
    o += sprintf(line + o, "+\033[0m");
    put_at(TAB_ROW + 1 + GH, tab_col, line);

    game_menu("w/a/s/d or arrows turn  p pause  q quit");
    printf("\033[?25l");
    fflush(stdout);
}

/* Advance one step. Returns false when the snake dies. */
static bool step(void)
{
    dx = pending_dx;
    dy = pending_dy;

    int nx = sx[head] + dx;
    int ny = sy[head] + dy;

    if (nx < 0 || nx >= GW || ny < 0 || ny >= GH)
        return false;

    bool eat = (nx == fx && ny == fy);

    /* The tail leaves its cell this step unless the snake grows, so a move
     * into the current tail cell stays legal. */
    int skip = (!eat && len > 0) ? len - 1 : -1;

    for (int i = 0; i < len; i++) {
        if (i == skip)
            continue;
        if (sx[cell_index(i)] == nx && sy[cell_index(i)] == ny)
            return false;
    }

    head = (head + 1) % (GW * GH);
    sx[head] = nx;
    sy[head] = ny;

    if (eat) {
        len++;
        score += 10;
        place_food();
    }
    return true;
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

    bool paused = false;
    bool dead = false;
    long last = game_ms();

    for (;;) {
        draw();

        if (dead)
            game_center(TAB_ROW + GH + 2,
                        "\033[1;31mGame over.  r restart  q quit\033[0m");
        else if (paused)
            game_center(TAB_ROW + GH + 2, "\033[1;33mPaused\033[0m");
        fflush(stdout);

        long speed = 140 - (score / 50) * 5;

        if (speed < 60)
            speed = 60;

        int c = game_read_key(10);

        if (c == 'q')
            break;
        if (c == 'r') {
            reset();
            dead = false;
            paused = false;
            last = game_ms();
            continue;
        }
        if (c == 'p') {
            paused = !paused;
            last = game_ms();
            continue;
        }

        if (!dead && !paused) {
            int ndx = pending_dx, ndy = pending_dy;

            if (c == 'a' || c == KEY_LEFT)
                ndx = -1, ndy = 0;
            else if (c == 'd' || c == KEY_RIGHT)
                ndx = 1, ndy = 0;
            else if (c == 'w' || c == KEY_UP)
                ndx = 0, ndy = -1;
            else if (c == 's' || c == KEY_DOWN)
                ndx = 0, ndy = 1;

            if (ndx != -dx || ndy != -dy) {
                pending_dx = ndx;
                pending_dy = ndy;
            }

            long now = game_ms();

            if (now - last >= speed) {
                last = now;
                if (!step())
                    dead = true;
            }
        }
    }

    printf("\033[2J\033[?25h\033[H");
    fflush(stdout);
    return 0;
}
