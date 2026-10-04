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
#include <unistd.h>

#include <sysfunc.h>

/* *INDENT-OFF* */
static command_help_t help_msg[] = {
    {"<help> ls",       "List directory contents."},
};
/* *INDENT-ON* */

static int32_t opt_all;
static int32_t opt_one;
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

static int32_t entry_color(uint8_t type)
{
    if (!use_color)
        return 0;
    if (type == DT_DIR)
        return 34;              /* blue */
    if (type == DT_LNK)
        return 36;              /* cyan */
    return 0;
}

static void print_name(const char *name, uint8_t type)
{
    int32_t color = entry_color(type);

    if (color != 0)
        printf("\033[%dm%s\033[0m", color, name);
    else
        printf("%s", name);
}

static void print_columns(char **names, uint8_t * types, size_t n)
{
    int32_t width = term_columns();

    if (n == 0)
        return;

    if (width <= 0) {
        for (size_t i = 0; i < n; i++) {
            print_name(names[i], types[i]);
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

            print_name(names[idx], types[idx]);

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
    uint8_t *types = NULL;
    size_t n = 0, cap = 0;
    struct dirent *de;

    while ((de = readdir(dir)) != NULL) {
        if (!opt_all && de->d_name[0] == '.')
            continue;

        if (n == cap) {
            size_t ncap = cap ? cap * 2 : 64;
            char **nn = realloc(names, ncap * sizeof(*names));
            uint8_t *nt = realloc(types, ncap);

            if (nn == NULL || nt == NULL) {
                free(nn);
                free(nt);
                break;
            }
            names = nn;
            types = nt;
            cap = ncap;
        }
        names[n] = strdup(de->d_name);
        types[n] = de->d_type;
        n++;
    }
    closedir(dir);

    qsort(names, n, sizeof(*names), cmp_name);
    print_columns(names, types, n);

    for (size_t i = 0; i < n; i++)
        free(names[i]);
    free(names);
    free(types);
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
            print_name(paths[i], S_ISLNK(st.st_mode) ? DT_LNK : DT_REG);
            putchar('\n');
        }
        first = 0;
    }

    return 0;
}
