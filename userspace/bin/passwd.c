/* Change the current user's password in the shadow file. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <passwd.h>

int main(void)
{
    uint32_t uid = (uint32_t) getuid();
    char user[64];
    char hash[256];

    if (!pw_name_by_uid(uid, user, sizeof(user))) {
        printf("passwd: no passwd entry for uid %u\n", uid);
        return 1;
    }
    if (!pw_shadow_get(user, hash, sizeof(hash)))
        hash[0] = '\0';

    if (hash[0] != '\0') {
        char *old = getpass("Current password: ");

        if (old == NULL || !pw_verify(hash, old)) {
            printf("passwd: authentication failed\n");
            return 1;
        }
    }

    char *a = getpass("New password: ");
    char first[256] = "";

    if (a != NULL)
        snprintf(first, sizeof(first), "%s", a);
    char *b = getpass("Retype new password: ");
    char second[256] = "";

    if (b != NULL)
        snprintf(second, sizeof(second), "%s", b);
    if (first[0] == '\0') {
        printf("passwd: empty password rejected\n");
        return 1;
    }
    if (strcmp(first, second) != 0) {
        printf("passwd: passwords do not match\n");
        return 1;
    }
    char nh[256];

    pw_hash_new(first, nh, sizeof(nh));
    if (!pw_shadow_set(user, nh)) {
        printf("passwd: cannot write %s\n", PW_SHADOW_PATH);
        return 1;
    }
    printf("passwd: password updated\n");
    return 0;
}
