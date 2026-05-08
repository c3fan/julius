#ifndef CORE_SHA256_H
#define CORE_SHA256_H

/**
 * @file
 * SHA-256 hash functions (FIPS 180-4).
 */

#include <stddef.h>
#include <stdint.h>

#define SHA256_DIGEST_SIZE 32
#define SHA256_HEX_SIZE 65  /**< 64 hex chars + NUL terminator */

typedef struct {
    uint32_t state[8];
    uint64_t total_bytes;
    uint8_t buf[64];
    int buflen;
} sha256_ctx;

/** Initialize a SHA-256 context. */
void sha256_init(sha256_ctx *ctx);

/** Feed data into the SHA-256 context. */
void sha256_update(sha256_ctx *ctx, const void *data, size_t len);

/** Finalize and write 32-byte raw digest. ctx must not be used afterwards. */
void sha256_final(sha256_ctx *ctx, uint8_t digest[SHA256_DIGEST_SIZE]);

/**
 * Convert 32-byte raw digest to a 64-character lower-case hex string.
 * @param digest  Raw 32-byte digest
 * @param hex     Output buffer of at least SHA256_HEX_SIZE bytes
 */
void sha256_hex(const uint8_t digest[SHA256_DIGEST_SIZE], char hex[SHA256_HEX_SIZE]);

#endif /* CORE_SHA256_H */
