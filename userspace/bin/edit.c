/**-----------------------------------------------------------------------------

 @file    edit.c
 @brief   A small full-screen text editor for HanOS

 @details
 @verbatim

   A kilo-style editor: the whole file lives in one buffer, a line index maps a
   row to its offset, and every change rewrites the screen in place. Raw mode
   and ANSI cursor addressing drive the tty. Keys: arrows move, printable keys
   insert, Backspace deletes, Enter splits a line, Ctrl-S saves, Ctrl-Q quits.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <sysfunc.h>

static command_help_t help_msg[] = {
    {"<help> edit", "Edit a text file."},
};

static struct termios orig;
static int rows = 24, cols = 80;

static char *buf;
static long buflen, bufcap;
static long *lstart;            /* offset of the start of each line */
static long nlines;
static long cy, cx;             /* cursor: row and column */
static long rowoff, coloff;     /* first visible row and column */
static char *filename;
static int dirty;

static void die(const char *s)
{
    dprintf(STDERR, "edit: ");
    dprintf(STDERR, s);
    dprintf(STDERR, "\n");
    sys_exit(1);
}

static void raw_on(void)
{
    tcgetattr(STDIN_FILENO, &orig);
    struct termios t = orig;

    t.c_lflag &= ~(unsigned) (ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
}

static void raw_off(void)
{
    tcsetattr(STDIN_FILENO, TCSANOW, &orig);
    printf("\033[2J\033[?25h\033[H");
    fflush(stdout);
}

static void size(void)
{
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row && ws.ws_col) {
        rows = ws.ws_row;
        cols = ws.ws_col;
    }
}

static void rebuild(void)
{
    nlines = 0;
    lstart[nlines++] = 0;
    for (long i = 0; i < buflen; i++)
        if (buf[i] == '\n')
            lstart[nlines++] = i + 1;
}

static long line_len(long row)
{
    if (row < 0 || row >= nlines)
        return 0;
    long end = (row + 1 < nlines) ? lstart[row + 1] - 1 : buflen;

    return end - lstart[row];
}

/* Make room for one more byte at position at. */
static void ins_at(long at, const char *s, long n)
{
    if (buflen + n + 1 > bufcap) {
        bufcap = (bufcap == 0) ? 4096 : bufcap * 2;
        while (bufcap < buflen + n + 1)
            bufcap *= 2;
        buf = realloc(buf, bufcap);
        lstart = realloc(lstart, (bufcap + 1) * sizeof(long));
    }
    memmove(buf + at + n, buf + at, buflen - at);
    memcpy(buf + at, s, n);
    buflen += n;
    dirty = 1;
}

static void del_at(long at, long n)
{
    if (at >= buflen)
        return;
    if (at + n > buflen)
        n = buflen - at;
    memmove(buf + at, buf + at + n, buflen - at - n);
    buflen -= n;
    dirty = 1;
}

static void clamp(void)
{
    if (cy < 0)
        cy = 0;
    if (cy >= nlines)
        cy = nlines - 1;
    if (cx < 0)
        cx = 0;
    if (cx > line_len(cy))
        cx = line_len(cy);
}

static void draw(void)
{
    char *out = malloc((size_t) cols * 64 + 256);
    int o = 0;

    o += sprintf(out + o, "\033[?25l\033[H");

    if (cy < rowoff)
        rowoff = cy;
    if (cy >= rowoff + rows - 2)
        rowoff = cy - (rows - 2) + 1;
    if (cx < coloff)
        coloff = cx;
    if (cx >= coloff + cols)
        coloff = cx - cols + 1;

    int textrows = rows - 2;

    for (int r = 0; r < textrows; r++) {
        long row = rowoff + r;

        if (row >= nlines) {
            o += sprintf(out + o, "\033[90m~\033[0m\033[K\r\n");
            continue;
        }
        long ll = line_len(row);
        long from = (coloff < ll) ? coloff : ll;
        long n = ll - from;

        if (n > cols)
            n = cols;
        memcpy(out + o, buf + lstart[row] + from, (size_t) n);
        o += (int) n;
        o += sprintf(out + o, "\033[K\r\n");
    }

    /* Status line. */
    o += sprintf(out + o, "\033[7m\033[K%s%s  %ld,%ld  %ld lines\033[0m\r\n",
                 filename, dirty ? " [+]" : "", cy + 1, cx + 1, nlines);
    o += sprintf(out + o,
                 "\033[Karrows move  Ctrl-S save  Ctrl-Q quit  Backspace del");

    /* Place the cursor. */
    o += sprintf(out + o, "\033[%d;%dH\033[?25h", (int) (cy - rowoff) + 1,
                 (int) (cx - coloff) + 1);

    fwrite(out, 1, (size_t) o, stdout);
    fflush(stdout);
    free(out);
}

/* Read one key, decoding the ESC [ A/B/C/D arrows. Returns -1 on end. */
static int read_key(void)
{
    unsigned char c;

    if (read(STDIN_FILENO, &c, 1) != 1)
        return -1;
    if (c != 0x1b)
        return c;

    unsigned char a, b;

    if (read(STDIN_FILENO, &a, 1) != 1)
        return 0x1b;
    if (a != '[') {
        return 0x1b;
    }
    if (read(STDIN_FILENO, &b, 1) != 1)
        return 0x1b;
    switch (b) {
    case 'A':
        return 0x100;
    case 'B':
        return 0x101;
    case 'C':
        return 0x102;
    case 'D':
        return 0x103;
    }
    return 0x1b;
}

static void save(void)
{
    int fd = sys_open(filename, O_RDWR | O_CREAT);

    if (fd < 0)
        return;
    lseek(fd, 0, 0);
    ftruncate(fd, buflen);
    if (buflen > 0)
        sys_write(fd, buf, (size_t) buflen);
    sys_close(fd);
    dirty = 0;
}

static void load(void)
{
    bufcap = 4096;
    buf = malloc((size_t) bufcap);
    lstart = malloc((size_t) (bufcap + 1) * sizeof(long));
    buflen = 0;

    int fd = sys_open(filename, O_RDWR | O_CREAT);

    if (fd < 0)
        die("cannot open");
    for (;;) {
        if (buflen + 4096 > bufcap) {
            bufcap *= 2;
            buf = realloc(buf, (size_t) bufcap);
            lstart = realloc(lstart, (size_t) (bufcap + 1) * sizeof(long));
        }
        long n = sys_read(fd, buf + buflen, 4096);

        if (n <= 0)
            break;
        buflen += n;
    }
    sys_close(fd);
    buf[buflen] = '\0';
    rebuild();
    cx = cy = 0;
    rowoff = coloff = 0;
}

int main(int argc, char *argv[])
{
    if (argc < 2)
        die("usage: edit <file>");
    filename = argv[1];

    load();
    raw_on();
    atexit(raw_off);
    size();

    for (;;) {
        clamp();
        draw();

        int c = read_key();

        if (c == 0x11)          /* Ctrl-Q */
            break;
        if (c == 0x13) {        /* Ctrl-S */
            save();
            continue;
        }
        if (c == 0x100) {       /* up */
            cy--;
        } else if (c == 0x101) {        /* down */
            cy++;
        } else if (c == 0x102) {        /* right */
            cx++;
        } else if (c == 0x103) {        /* left */
            cx--;
        } else if (c == 0x7f || c == 8) {       /* backspace */
            if (cx > 0) {
                del_at(lstart[cy] + cx - 1, 1);
                cx--;
                rebuild();
            } else if (cy > 0) {
                long at = lstart[cy] - 1;

                cx = line_len(cy - 1);
                del_at(at, 1);
                cy--;
                rebuild();
            }
        } else if (c == '\r' || c == '\n') {
            ins_at(lstart[cy] + cx, "\n", 1);
            cy++;
            cx = 0;
            rebuild();
        } else if (c >= 0x20 && c < 0x7f) {
            char ch = (char) c;

            ins_at(lstart[cy] + cx, &ch, 1);
            cx++;
            rebuild();
        }
    }

    raw_off();
    return 0;
}
