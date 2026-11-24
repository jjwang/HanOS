/**-----------------------------------------------------------------------------

 @file    login.c
 @brief   Authenticate a console user and start the shell.

 **-----------------------------------------------------------------------------
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

#include <passwd.h>

static void read_name(char *buf, size_t size)
{
    write(1, "HanOS login:", 12);
    write(1, " ", 1);
    if (fgets(buf, (int) size, stdin) != NULL) {
        char *nl = strchr(buf, '\n');

        if (nl != NULL)
            *nl = '\0';
    }
    if (buf[0] == '\0')
        snprintf(buf, size, "root");
}

static const char *pick_shell(const char *shell)
{
    if (shell != NULL && shell[0] != '\0' && access(shell, X_OK) == 0)
        return shell;
    if (access("/data/bin/bash", X_OK) == 0)
        return "/data/bin/bash";
    if (access("/bin/bash", X_OK) == 0)
        return "/bin/bash";
    return "/bin/hansh";
}

int main(void)
{
    char user[64];
    uint32_t uid = 0;
    uint32_t gid = 0;
    char shell[128] = "/bin/bash";
    char hash[256];

    read_name(user, sizeof(user));
    if (!pw_lookup(user, &uid, &gid, shell, sizeof(shell))) {
        printf("login: unknown user %s\n", user);
        return 1;
    }

    if (!pw_shadow_get(user, hash, sizeof(hash)))
        hash[0] = '\0';

    if (hash[0] == '\0') {
        /* First login for this user: force a password. */
        printf("Password not set for %s. Set it now.\n", user);
        char first[256];
        char second[256];

        pw_read_password("New password: ", first, sizeof(first));
        pw_read_password("Retype new password: ", second, sizeof(second));
        if (first[0] == '\0') {
            printf("login: empty password rejected\n");
            return 1;
        }
        if (strcmp(first, second) != 0) {
            printf("login: passwords do not match\n");
            return 1;
        }
        char nh[256];

        pw_hash_new(first, nh, sizeof(nh));
        if (!pw_shadow_set(user, nh))
            printf("login: cannot save the password to %s\n", PW_SHADOW_PATH);
        else
            printf("login: password set\n");
    } else {
        int tries;

        for (tries = 0; tries < 3; tries++) {
            char p[256];

            pw_read_password("Password: ", p, sizeof(p));
            if (pw_verify(hash, p))
                break;
            printf("Login incorrect\n");
        }
        if (tries == 3)
            return 1;
    }

    if (setgid(gid) != 0 || setuid(uid) != 0) {
        printf("login: cannot switch to %u:%u\n", uid, gid);
        return 1;
    }

    const char *sh = pick_shell(shell);
    char *argv[2] = { (char *) sh, NULL };

    execv(sh, argv);
    printf("login: cannot exec %s\n", sh);
    return 1;
}
