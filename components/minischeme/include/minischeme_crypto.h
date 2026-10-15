/**
 * @file minischeme_crypto.h
 * @brief The few primitives the encrypted channel needs (minischeme_channel):
 *        SHA-256, HMAC-SHA256, HKDF-SHA256 (RFC 5869) and AES-256-GCM with a
 *        96-bit nonce and a 128-bit tag (NIST SP 800-38D).
 *
 * On the ESP32, AES-GCM is PSA Crypto's (mbedTLS 4 in ESP-IDF 6): its ESP
 * driver runs GCM on the AES hardware accelerator, a whole record per call.
 * Elsewhere (host tests, WASM) a portable software AES and a table-free
 * GHASH; the tests check it against the Python converter's.
 */

#ifndef MINISCHEME_CRYPTO_H
#define MINISCHEME_CRYPTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(ESP_PLATFORM)
#include "psa/crypto.h"
#endif

typedef struct {
    uint32_t h[8];
    uint64_t bytes;
    uint8_t buf[64];
    size_t used;
} MiniSha256;

void mini_sha256_init(MiniSha256 *s);
void mini_sha256_update(MiniSha256 *s, const void *data, size_t n);
void mini_sha256_final(MiniSha256 *s, uint8_t out[32]);
void mini_sha256(const void *data, size_t n, uint8_t out[32]);

void mini_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len, uint8_t out[32]);
/** @brief HKDF-SHA256: out_len (up to 8160) bytes from ikm, salt and info. */
void mini_hkdf_sha256(const uint8_t *salt, size_t salt_len, const uint8_t *ikm, size_t ikm_len,
                      const uint8_t *info, size_t info_len, uint8_t *out, size_t out_len);

typedef struct {
#if defined(ESP_PLATFORM)
    psa_key_id_t key;  /* a volatile PSA key */
#else
    uint8_t rk[240];   /* AES-256 round keys */
    uint8_t h[16];     /* the GHASH key E(K, 0) */
#endif
} MiniGcm;

/** @brief A 256-bit key. */
void mini_gcm_init(MiniGcm *g, const uint8_t key[32]);
void mini_gcm_free(MiniGcm *g);
/** @brief Encrypts the n bytes of data in place and writes the 16-byte tag
 *         right after them (data has n + 16 bytes of room). */
void mini_gcm_seal(MiniGcm *g, const uint8_t nonce[12], const uint8_t *aad, size_t aad_len, uint8_t *data, size_t n);
/** @brief data: n bytes of ciphertext, then the tag. Checks the tag, then
 *         decrypts in place; false when the tag does not match. */
bool mini_gcm_open(MiniGcm *g, const uint8_t nonce[12], const uint8_t *aad, size_t aad_len, uint8_t *data, size_t n);

/** @brief Overwrites n bytes the compiler may not drop (keys on the stack). */
void mini_wipe(void *p, size_t n);

#endif /* MINISCHEME_CRYPTO_H */
