#pragma once

#include <stddef.h>
#include <stdint.h>

// Minimal primitives for the vault: SHA-256, HMAC-SHA256, PBKDF2 and a stream cipher
// built from HMAC-SHA256 in counter mode. Small and dependency-free (the SDK's mbedtls
// submodule is usually not checked out).

typedef struct {
    uint32_t h[8];
    uint64_t len;
    uint8_t buf[64];
    size_t fill;
} sha256_ctx_t;

void sha256_init(sha256_ctx_t *c);
void sha256_update(sha256_ctx_t *c, const void *data, size_t n);
void sha256_final(sha256_ctx_t *c, uint8_t out[32]);

typedef struct {
    sha256_ctx_t inner, outer;
} hmac_ctx_t;

void hmac_init(hmac_ctx_t *c, const void *key, size_t keylen);
void hmac_update(hmac_ctx_t *c, const void *data, size_t n);
void hmac_final(hmac_ctx_t *c, uint8_t out[32]);
void hmac_sha256(const void *key, size_t keylen, const void *data, size_t n, uint8_t out[32]);

void pbkdf2_sha256(const void *pass, size_t passlen, const uint8_t *salt, size_t saltlen,
                   uint32_t iterations, uint8_t *out, size_t outlen);

// XORs data with HMAC-SHA256(key, nonce || block counter). Encrypts and decrypts.
void stream_xor(const uint8_t key[32], const uint8_t nonce[16], uint8_t *data, size_t n);

// Constant time; returns 0 when equal.
int ct_compare(const void *a, const void *b, size_t n);
void secure_zero(void *p, size_t n);
