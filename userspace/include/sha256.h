#ifndef HANOS_SHA256_H
#define HANOS_SHA256_H

#include <stddef.h>
#include <stdint.h>

/** @brief SHA-256 hashing state. */
typedef struct {
    uint32_t h[8];
    uint64_t len;
    uint8_t buf[64];
    size_t n;
} sha256_ctx_t;

void sha256_init(sha256_ctx_t *c);
void sha256_update(sha256_ctx_t *c, const void *data, size_t len);
void sha256_final(sha256_ctx_t *c, uint8_t out[32]);
void sha256_hex(const void *data, size_t len, char out[65]);

#endif
