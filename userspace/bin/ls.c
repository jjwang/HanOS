/**-----------------------------------------------------------------------------

 @file    ls.c
 @brief   List directory contents

 @details
 @verbatim

   Prints the entries of each directory argument sorted by name. On a
   terminal it lays the names out in columns sized to the terminal width
   and colors directories and symlinks; through a pipe it prints one name
   per line. Hidden names are skipped unless -a; -1 forces one per line.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stddef.h>
#include <stdint.h>

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <sysfunc.h>

/* *INDENT-OFF* */
static command_help_t help_msg[] = {
    {"<help> ls",       "List directory contents."},
};
/* *INDENT-ON* */

static int32_t opt_all;
static int32_t opt_one;
static int32_t opt_long;
static int32_t use_color;

static int32_t cmp_name(const void *a, const void *b)
{
    return strcmp(*(char *const *) a, *(char *const *) b);
}

static int32_t term_columns(void)
{
    struct winsize ws;

    if (opt_one || !isatty(STDOUT_FILENO))
        return 0;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 || ws.ws_col == 0)
        return 80;
    return ws.ws_col;
}

static int32_t entry_color_for(const char *fullpath, uint8_t type)
{
    if (!use_color)
        return 0;
    if (type == DT_DIR)
        return 34;              /* blue */
    if (type == DT_LNK)
        return 36;              /* cyan */
    if (type == DT_REG) {
        struct stat st;

        if (stat(fullpath, &st) == 0 && (st.st_mode & 0111))
            return 32;          /* green: executable */
    }
    return 0;
}

static void print_name(const char *name, int32_t color)
{
    if (color != 0)
        printf("\033[%dm%s\033[0m", color, name);
    else
        printf("%s", name);
}

/* Build the 10-character mode string, e.g. "-rw-r--r--". */
static void mode_string(mode_t mode, char *out)
{
    static const char rwx[] = "rwx";

    out[0] = S_ISDIR(mode) ? 'd' :
        S_ISLNK(mode) ? 'l' :
        S_ISCHR(mode) ? 'c' :
        S_ISBLK(mode) ? 'b' :
        S_ISFIFO(mode) ? 'p' : S_ISSOCK(mode) ? 's' : '-';

    for (int32_t i = 0; i < 9; i++) {
        out[i + 1] = (mode & (1 << (8 - i))) ? rwx[i % 3] : '-';
    }

    if (mode & S_ISUID)
        out[3] = (out[3] == 'x') ? 's' : 'S';
    if (mode & S_ISGID)
        out[6] = (out[6] == 'x') ? 's' : 'S';
    if (mode & S_ISVTX)
        out[9] = (out[9] == 'x') ? 't' : 'T';
    out[10] = '\0';
}

static void print_long(const char *path, const char *name, int32_t color)
{
    struct stat st;

    if (lstat(path, &st) != 0)
        return;

    char perms[11];
    char tbuf[32];
    struct tm tmv;

    mode_string(st.st_mode, perms);
    memset(&tmv, 0, sizeof(tmv));
    gmtime_r(&st.st_mtim.tv_sec, &tmv);
    strftime(tbuf, sizeof(tbuf), "%b %e %H:%M", &tmv);

    printf("%s %2lu %u %u %8lld %s ", perms, (unsigned long) st.st_nlink,
           (unsigned) st.st_uid, (unsigned) st.st_gid,
           (long long) st.st_size, tbuf);
    print_name(name, color);
    putchar('\n');
}

static void print_columns(char **names, int32_t *colors, size_t n)
{
    int32_t width = term_columns();

    if (n == 0)
        return;

    if (width <= 0) {
        for (size_t i = 0; i < n; i++) {
            print_name(names[i], colors[i]);
            putchar('\n');
        }
        return;
    }

    size_t maxlen = 0;

    for (size_t i = 0; i < n; i++) {
        size_t l = strlen(names[i]);

        if (l > maxlen)
            maxlen = l;
    }

    size_t cell = maxlen + 2;
    size_t cols = (size_t) width / cell;

    if (cols == 0)
        cols = 1;

    size_t rows = (n + cols - 1) / cols;

    for (size_t r = 0; r < rows; r++) {
        for (size_t c = 0; c < cols; c++) {
            size_t idx = c * rows + r;

            if (idx >= n)
                continue;

            print_name(names[idx], colors[idx]);

            if (c + 1 < cols && idx + rows < n) {
                size_t pad = cell - strlen(names[idx]);

                while (pad-- > 0)
                    putchar(' ');
            }
        }
        putchar('\n');
    }
}

static void list_dir(const char *path)
{
    DIR *dir = opendir(path);

    if (dir == NULL) {
        fprintf(stderr, "ls: cannot access '%s': %s\n", path,
                strerror(errno));
        return;
    }

    char **names = NULL;
    int32_t *colors = NULL;
    size_t n = 0, cap = 0;
    struct dirent *de;

    while ((de = readdir(dir)) != NULL) {
        if (!opt_all && de->d_name[0] == '.')
            continue;

        if (n == cap) {
            size_t ncap = cap ? cap * 2 : 64;
            char **nn = realloc(names, ncap * sizeof(*names));
            int32_t *nc = realloc(colors, ncap * sizeof(*colors));

            if (nn == NULL || nc == NULL) {
                free(nn);
                free(nc);
                break;
            }
            names = nn;
            colors = nc;
            cap = ncap;
        }
        names[n] = strdup(de->d_name);
        char full[4096];

        snprintf(full, sizeof(full), "%s/%s", path, de->d_name);
        colors[n] = entry_color_for(full, de->d_type);
        n++;
    }
    closedir(dir);

    qsort(names, n, sizeof(*names), cmp_name);

    if (opt_long) {
        for (size_t i = 0; i < n; i++) {
            char full[4096];

            snprintf(full, sizeof(full), "%s/%s", path, names[i]);
            print_long(full, names[i], colors[i]);
        }
    } else {
        print_columns(names, colors, n);
    }

    for (size_t i = 0; i < n; i++)
        free(names[i]);
    free(names);
    free(colors);
}

int32_t main(int32_t argc, char *argv[])
{
    const char *paths[64];
    int32_t npaths = 0;
    int32_t first = 1;

    use_color = isatty(STDOUT_FILENO);

    for (int32_t i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            for (int32_t j = 1; argv[i][j] != '\0'; j++) {
                if (argv[i][j] == 'a')
                    opt_all = 1;
                else if (argv[i][j] == '1')
                    opt_one = 1;
                else if (argv[i][j] == 'l')
                    opt_long = 1;
                else
                    fprintf(stderr, "ls: invalid option -- '%c'\n",
                            argv[i][j]);
            }
            continue;
        }
        if (npaths < 64)
            paths[npaths++] = argv[i];
    }

    if (npaths == 0)
        paths[npaths++] = ".";

    for (int32_t i = 0; i < npaths; i++) {
        struct stat st;

        if (lstat(paths[i], &st) != 0) {
            fprintf(stderr, "ls: cannot access '%s': %s\n", paths[i],
                    strerror(errno));
            first = 0;
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            if (npaths > 1) {
                if (!first)
                    putchar('\n');
                printf("%s:\n", paths[i]);
            }
            list_dir(paths[i]);
        } else {
            uint8_t type = S_ISLNK(st.st_mode) ? DT_LNK : DT_REG;
            int32_t color = entry_color_for(paths[i], type);

            if (opt_long) {
                print_long(paths[i], paths[i], color);
            } else {
                print_name(paths[i], color);
                putchar('\n');
            }
        }
        first = 0;
    }

    return 0;
}
