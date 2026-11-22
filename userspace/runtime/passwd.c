/* Password hashing and the ext2 shadow file for the login helper. */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/random.h>
#include <termios.h>
#include <unistd.h>

#include <passwd.h>
#include <sha256.h>

#define PW_MAX_PASS 256
#define PW_SHADOW_MAX 4096

static void salt_hex(const unsigned char *b, char out[17])
{
    static const char h[] = "0123456789abcdef";

    for (int i = 0; i < 8; i++) {
        out[i * 2] = h[b[i] >> 4];
        out[i * 2 + 1] = h[b[i] & 0xf];
    }
    out[16] = '\0';
}

bool pw_lookup(const char *user, uint32_t *uid, uint32_t *gid,
               char *shell, size_t shell_len)
{
    FILE *f = fopen(PW_PASSWD_PATH, "r");
    char line[256];
    bool ok = false;

    if (f == NULL)
        return false;

    while (fgets(line, sizeof(line), f) != NULL) {
        char *fields[7];
        char *s = line;
        char *tok;
        char *nl;
        int n = 0;

        nl = strchr(line, '\n');
        if (nl != NULL)
            *nl = '\0';
        while (n < 7 && (tok = strsep(&s, ":")) != NULL)
            fields[n++] = tok;
        if (n < 7 || strcmp(fields[0], user) != 0)
            continue;
        if (uid != NULL)
            *uid = (uint32_t) strtoul(fields[2], NULL, 10);
        if (gid != NULL)
            *gid = (uint32_t) strtoul(fields[3], NULL, 10);
        if (shell != NULL)
            snprintf(shell, shell_len, "%s", fields[6]);
        ok = true;
        break;
    }
    fclose(f);
    return ok;
}

bool pw_name_by_uid(uint32_t uid, char *out, size_t out_len)
{
    FILE *f = fopen(PW_PASSWD_PATH, "r");
    char line[256];

    if (f == NULL)
        return false;
    while (fgets(line, sizeof(line), f) != NULL) {
        char *fields[7];
        char *s = line;
        char *tok;
        char *nl = strchr(line, '\n');
        int n = 0;

        if (nl != NULL)
            *nl = '\0';
        while (n < 7 && (tok = strsep(&s, ":")) != NULL)
            fields[n++] = tok;
        if (n < 7 || (uint32_t) strtoul(fields[2], NULL, 10) != uid)
            continue;
        snprintf(out, out_len, "%s", fields[0]);
        fclose(f);
        return true;
    }
    fclose(f);
    return false;
}

bool pw_shadow_get(const char *user, char *hash, size_t hash_len)
{
    FILE *f;
    char line[256];
    size_t ulen = strlen(user);
    bool found = false;

    if (hash_len == 0)
        return false;
    hash[0] = '\0';

    f = fopen(PW_SHADOW_PATH, "r");
    if (f == NULL)
        return false;

    while (fgets(line, sizeof(line), f) != NULL) {
        char *nl = strchr(line, '\n');
        const char *h;
        const char *e;
        size_t n;

        if (nl != NULL)
            *nl = '\0';
        if (strncmp(line, user, ulen) != 0 || line[ulen] != ':')
            continue;
        h = line + ulen + 1;
        e = strchr(h, ':');
        n = (e != NULL) ? (size_t) (e - h) : strlen(h);
        if (n >= hash_len)
            n = hash_len - 1;
        memcpy(hash, h, n);
        hash[n] = '\0';
        found = true;
        break;
    }
    fclose(f);
    return found;
}

bool pw_shadow_set(const char *user, const char *hash)
{
    char buf[PW_SHADOW_MAX];
    size_t used = 0;
    size_t ulen = strlen(user);
    bool done = false;
    FILE *f = fopen(PW_SHADOW_PATH, "r");

    if (f != NULL) {
        char line[256];

        while (fgets(line, sizeof(line), f) != NULL) {
            char *nl = strchr(line, '\n');
            size_t n;

            if (nl != NULL)
                *nl = '\0';
            n = strlen(line);
            if (strncmp(line, user, ulen) == 0 && line[ulen] == ':') {
                int w = snprintf(buf + used, sizeof(buf) - used,
                                 "%s:%s:0:0:99999:7:::\n", user, hash);

                if (w > 0 && used + (size_t) w < sizeof(buf))
                    used += (size_t) w;
                done = true;
            } else if (n > 0 && used + n + 2 < sizeof(buf)) {
                memcpy(buf + used, line, n);
                used += n;
                buf[used++] = '\n';
            }
        }
        fclose(f);
    }

    if (!done) {
        int w = snprintf(buf + used, sizeof(buf) - used,
                         "%s:%s:0:0:99999:7:::\n", user, hash);

        if (w > 0 && used + (size_t) w < sizeof(buf))
            used += (size_t) w;
    }

    f = fopen(PW_SHADOW_PATH, "w");
    if (f == NULL)
        return false;
    size_t w = fwrite(buf, 1, used, f);

    fclose(f);
    return w == used;
}

void pw_hash_new(const char *password, char *out, size_t out_len)
{
    unsigned char r[8];
    char salt[17];
    char combo[17 + PW_MAX_PASS];
    char digest[65];
    size_t sl;
    size_t pl = strlen(password);

    if (getrandom(r, sizeof(r), 0) != (ssize_t) sizeof(r)) {
        for (size_t i = 0; i < sizeof(r); i++)
            r[i] = (unsigned char) (i * 37 + 13);
    }
    salt_hex(r, salt);
    if (pl > PW_MAX_PASS)
        pl = PW_MAX_PASS;
    sl = strlen(salt);
    memcpy(combo, salt, sl);
    memcpy(combo + sl, password, pl);
    sha256_hex(combo, sl + pl, digest);
    snprintf(out, out_len, "$5$%s$%s", salt, digest);
}

bool pw_verify(const char *stored, const char *password)
{
    const char *salt;
    const char *dollar;
    const char *hex;
    char combo[17 + PW_MAX_PASS];
    char digest[65];
    size_t sl;
    size_t pl = strlen(password);

    if (stored == NULL || strncmp(stored, "$5$", 3) != 0)
        return false;
    salt = stored + 3;
    dollar = strchr(salt, '$');
    if (dollar == NULL)
        return false;
    sl = (size_t) (dollar - salt);
    hex = dollar + 1;
    if (sl == 0 || sl > 16)
        return false;
    if (pl > PW_MAX_PASS)
        pl = PW_MAX_PASS;
    memcpy(combo, salt, sl);
    memcpy(combo + sl, password, pl);
    sha256_hex(combo, sl + pl, digest);
    return strcmp(hex, digest) == 0;
}

void pw_read_password(const char *prompt, char *buf, size_t len)
{
    struct termios old;
    ssize_t n;
    int restore = 0;

    if (len == 0)
        return;
    write(1, prompt, strlen(prompt));
    if (tcgetattr(0, &old) == 0) {
        struct termios raw = old;

        raw.c_lflag &= ~ECHO;
        tcsetattr(0, TCSANOW, &raw);
        restore = 1;
    }
    n = read(0, buf, len - 1);
    if (n < 0)
        n = 0;
    if (n > 0 && buf[n - 1] == '\n')
        n--;
    buf[n] = '\0';
    if (restore)
        tcsetattr(0, TCSANOW, &old);
    write(1, "\n", 1);
}
