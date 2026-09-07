#ifndef XPDASH_SHA256_H
#define XPDASH_SHA256_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t state[8];
    uint64_t count;
    uint8_t buffer[64];
} sha256_ctx;

void sha256_init(sha256_ctx *ctx);
void sha256_update(sha256_ctx *ctx, const uint8_t *data, size_t len);
void sha256_final(sha256_ctx *ctx, uint8_t hash[32]);

/* One-shot SHA-256 hash calculation */
void sha256_hash(const uint8_t *data, size_t len, uint8_t hash[32]);

/* Format 32-byte hash as lowercase hex string: "SHA256:0123456789abcdef..." (71 chars + null) */
void sha256_format_fingerprint(const uint8_t hash[32], char out_str[72]);

/* Format 32-byte hash as bare lowercase hex string (64 chars + null) */
void sha256_format_hex(const uint8_t hash[32], char out_str[65]);

#endif /* XPDASH_SHA256_H */
