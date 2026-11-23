/**-----------------------------------------------------------------------------

 @file    passwd.h
 @brief   Password, shadow and prompt helpers shared by login and passwd.

 **-----------------------------------------------------------------------------
 */
#ifndef HANOS_PASSWD_H
#define HANOS_PASSWD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PW_SHADOW_PATH  "/data/etc/shadow"
#define PW_PASSWD_PATH  "/etc/passwd"

/* Look up a user in /etc/passwd. */
bool pw_lookup(const char *user, uint32_t *uid, uint32_t *gid,
               char *shell, size_t shell_len);

bool pw_name_by_uid(uint32_t uid, char *out, size_t out_len);

/* Read a user's stored hash from the shadow file. Empty when unset. */
bool pw_shadow_get(const char *user, char *hash, size_t hash_len);

/* Replace or append a user's hash in the shadow file. */
bool pw_shadow_set(const char *user, const char *hash);

/* Hash a password with a fresh salt into "$5$<salt>$<hex>". */
void pw_hash_new(const char *password, char *out, size_t out_len);

/* Verify a password against a stored "$5$..." hash. */
bool pw_verify(const char *stored, const char *password);

void pw_read_password(const char *prompt, char *buf, size_t len);

#endif
