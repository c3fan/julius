/*
 * SHA-256 implementation (FIPS 180-4).
 *
 * This is free and unencumbered software released into the public domain.
 */
#include "sha256.h"

#include <string.h>

/* Rotate right */
#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

/* SHA-256 logical functions */
#define CH(x, y, z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define S0(x) (ROTR((x),  2) ^ ROTR((x), 13) ^ ROTR((x), 22))
#define S1(x) (ROTR((x),  6) ^ ROTR((x), 11) ^ ROTR((x), 25))
#define s0(x) (ROTR((x),  7) ^ ROTR((x), 18) ^ ((x) >>  3))
#define s1(x) (ROTR((x), 17) ^ ROTR((x), 19) ^ ((x) >> 10))

/* SHA-256 round constants (FIPS 180-4 section 4.2.2) */
static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static void sha256_transform(sha256_ctx *ctx, const uint8_t block[64])
{
    uint32_t W[64];
    uint32_t a, b, c, d, e, f, g, h;
    int i;

    /* Prepare message schedule */
    for (i = 0; i < 16; i++) {
        W[i] = ((uint32_t)block[i * 4    ] << 24)
             | ((uint32_t)block[i * 4 + 1] << 16)
             | ((uint32_t)block[i * 4 + 2] <<  8)
             | ((uint32_t)block[i * 4 + 3]);
    }
    for (i = 16; i < 64; i++) {
        W[i] = s1(W[i - 2]) + W[i - 7] + s0(W[i - 15]) + W[i - 16];
    }

    a = ctx->state[0];  b = ctx->state[1];
    c = ctx->state[2];  d = ctx->state[3];
    e = ctx->state[4];  f = ctx->state[5];
    g = ctx->state[6];  h = ctx->state[7];

    for (i = 0; i < 64; i++) {
        uint32_t T1 = h + S1(e) + CH(e, f, g) + K[i] + W[i];
        uint32_t T2 = S0(a) + MAJ(a, b, c);
        h = g;  g = f;  f = e;  e = d + T1;
        d = c;  c = b;  b = a;  a = T1 + T2;
    }

    ctx->state[0] += a;  ctx->state[1] += b;
    ctx->state[2] += c;  ctx->state[3] += d;
    ctx->state[4] += e;  ctx->state[5] += f;
    ctx->state[6] += g;  ctx->state[7] += h;
}

void sha256_init(sha256_ctx *ctx)
{
    ctx->state[0] = 0x6a09e667u;
    ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u;
    ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu;
    ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu;
    ctx->state[7] = 0x5be0cd19u;
    ctx->total_bytes = 0;
    ctx->buflen = 0;
}

void sha256_update(sha256_ctx *ctx, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    ctx->total_bytes += (uint64_t)len;
    while (len > 0) {
        int n = 64 - ctx->buflen;
        if ((size_t)n > len) {
            n = (int)len;
        }
        memcpy(ctx->buf + ctx->buflen, p, (size_t)n);
        ctx->buflen += n;
        p   += n;
        len -= (size_t)n;
        if (ctx->buflen == 64) {
            sha256_transform(ctx, ctx->buf);
            ctx->buflen = 0;
        }
    }
}

void sha256_final(sha256_ctx *ctx, uint8_t digest[SHA256_DIGEST_SIZE])
{
    uint64_t bit_count = ctx->total_bytes * 8u;
    int i;

    /* Append the 0x80 padding byte */
    ctx->buf[ctx->buflen++] = 0x80;

    /* If there is not enough room for the 8-byte length, flush the block */
    if (ctx->buflen > 56) {
        memset(ctx->buf + ctx->buflen, 0, (size_t)(64 - ctx->buflen));
        sha256_transform(ctx, ctx->buf);
        ctx->buflen = 0;
    }

    /* Pad with zeros up to byte 56 */
    memset(ctx->buf + ctx->buflen, 0, (size_t)(56 - ctx->buflen));

    /* Append bit length in big-endian */
    ctx->buf[56] = (uint8_t)(bit_count >> 56);
    ctx->buf[57] = (uint8_t)(bit_count >> 48);
    ctx->buf[58] = (uint8_t)(bit_count >> 40);
    ctx->buf[59] = (uint8_t)(bit_count >> 32);
    ctx->buf[60] = (uint8_t)(bit_count >> 24);
    ctx->buf[61] = (uint8_t)(bit_count >> 16);
    ctx->buf[62] = (uint8_t)(bit_count >>  8);
    ctx->buf[63] = (uint8_t)(bit_count);
    sha256_transform(ctx, ctx->buf);

    /* Serialise state (big-endian) */
    for (i = 0; i < 8; i++) {
        digest[i * 4    ] = (uint8_t)(ctx->state[i] >> 24);
        digest[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        digest[i * 4 + 2] = (uint8_t)(ctx->state[i] >>  8);
        digest[i * 4 + 3] = (uint8_t)(ctx->state[i]);
    }
}

void sha256_hex(const uint8_t digest[SHA256_DIGEST_SIZE], char hex[SHA256_HEX_SIZE])
{
    static const char H[] = "0123456789abcdef";
    int i;
    for (i = 0; i < SHA256_DIGEST_SIZE; i++) {
        hex[i * 2    ] = H[digest[i] >> 4];
        hex[i * 2 + 1] = H[digest[i] & 0x0f];
    }
    hex[64] = '\0';
}
