/**
 * @file minischeme_crypto.c
 * @brief SHA-256, HMAC, HKDF and AES-256-GCM for the encrypted channel
 *        (see minischeme_crypto.h).
 */

#include "minischeme_crypto.h"
#include <string.h>

void mini_wipe(void *p, size_t n) {
    volatile uint8_t *b = (volatile uint8_t *)p;
    while (n--) *b++ = 0;
}

static uint32_t load_be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void store_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* ========================================================================= */
/* SHA-256 (FIPS 180-4)                                                       */
/* ========================================================================= */

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(MiniSha256 *s, const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = load_be32(p + 4 * i);
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR32(w[i - 15], 7) ^ ROR32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR32(w[i - 2], 17) ^ ROR32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROR32(e, 6) ^ ROR32(e, 11) ^ ROR32(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ROR32(a, 2) ^ ROR32(a, 13) ^ ROR32(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

void mini_sha256_init(MiniSha256 *s) {
    static const uint32_t iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memcpy(s->h, iv, sizeof(iv));
    s->bytes = 0;
    s->used = 0;
}

void mini_sha256_update(MiniSha256 *s, const void *data, size_t n) {
    const uint8_t *p = (const uint8_t *)data;
    s->bytes += n;
    while (n > 0) {
        size_t k = 64 - s->used < n ? 64 - s->used : n;
        memcpy(s->buf + s->used, p, k);
        s->used += k;
        p += k;
        n -= k;
        if (s->used == 64) {
            sha256_block(s, s->buf);
            s->used = 0;
        }
    }
}

void mini_sha256_final(MiniSha256 *s, uint8_t out[32]) {
    uint64_t bits = s->bytes * 8;
    uint8_t pad = 0x80;
    mini_sha256_update(s, &pad, 1);
    pad = 0;
    while (s->used != 56) mini_sha256_update(s, &pad, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
    mini_sha256_update(s, len, 8);
    for (int i = 0; i < 8; i++) store_be32(out + 4 * i, s->h[i]);
    mini_wipe(s, sizeof(*s));
}

void mini_sha256(const void *data, size_t n, uint8_t out[32]) {
    MiniSha256 s;
    mini_sha256_init(&s);
    mini_sha256_update(&s, data, n);
    mini_sha256_final(&s, out);
}

/* ========================================================================= */
/* HMAC-SHA256 (RFC 2104) and HKDF (RFC 5869)                                 */
/* ========================================================================= */

typedef struct {
    MiniSha256 inner;
    uint8_t opad[64];
} MiniHmac;

static void hmac_begin(MiniHmac *m, const uint8_t *key, size_t key_len) {
    uint8_t k[64];
    memset(k, 0, sizeof(k));
    if (key_len > 64) mini_sha256(key, key_len, k);
    else memcpy(k, key, key_len);
    uint8_t ipad[64];
    for (int i = 0; i < 64; i++) {
        ipad[i] = k[i] ^ 0x36;
        m->opad[i] = k[i] ^ 0x5c;
    }
    mini_sha256_init(&m->inner);
    mini_sha256_update(&m->inner, ipad, 64);
    mini_wipe(k, sizeof(k));
    mini_wipe(ipad, sizeof(ipad));
}

static void hmac_end(MiniHmac *m, uint8_t out[32]) {
    uint8_t ih[32];
    mini_sha256_final(&m->inner, ih);
    MiniSha256 outer;
    mini_sha256_init(&outer);
    mini_sha256_update(&outer, m->opad, 64);
    mini_sha256_update(&outer, ih, 32);
    mini_sha256_final(&outer, out);
    mini_wipe(ih, sizeof(ih));
    mini_wipe(m, sizeof(*m));
}

void mini_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len, uint8_t out[32]) {
    MiniHmac m;
    hmac_begin(&m, key, key_len);
    mini_sha256_update(&m.inner, msg, msg_len);
    hmac_end(&m, out);
}

void mini_hkdf_sha256(const uint8_t *salt, size_t salt_len, const uint8_t *ikm, size_t ikm_len,
                      const uint8_t *info, size_t info_len, uint8_t *out, size_t out_len) {
    static const uint8_t zeros[32] = {0};
    uint8_t prk[32], t[32];
    if (!salt || salt_len == 0) {
        salt = zeros;
        salt_len = sizeof(zeros);
    }
    mini_hmac_sha256(salt, salt_len, ikm, ikm_len, prk);
    size_t done = 0;
    for (uint8_t i = 1; done < out_len && i != 0; i++) {
        MiniHmac m;
        hmac_begin(&m, prk, sizeof(prk));
        if (i > 1) mini_sha256_update(&m.inner, t, sizeof(t));
        if (info_len) mini_sha256_update(&m.inner, info, info_len);
        mini_sha256_update(&m.inner, &i, 1);
        hmac_end(&m, t);
        size_t k = out_len - done < 32 ? out_len - done : 32;
        memcpy(out + done, t, k);
        done += k;
    }
    mini_wipe(prk, sizeof(prk));
    mini_wipe(t, sizeof(t));
}

/* ========================================================================= */
/* AES-256 block encryption                                                   */
/* ========================================================================= */

#if !defined(ESP_PLATFORM)
static const uint8_t SBOX[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};

static uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0)); }

static void aes256_expand(uint8_t rk[240], const uint8_t key[32]) {
    memcpy(rk, key, 32);
    uint8_t rcon = 1;
    for (int i = 8; i < 60; i++) {
        uint8_t t[4];
        memcpy(t, rk + (i - 1) * 4, 4);
        if (i % 8 == 0) {
            uint8_t u = t[0];
            t[0] = (uint8_t)(SBOX[t[1]] ^ rcon);
            t[1] = SBOX[t[2]];
            t[2] = SBOX[t[3]];
            t[3] = SBOX[u];
            rcon = xtime(rcon);
        } else if (i % 8 == 4) {
            for (int k = 0; k < 4; k++) t[k] = SBOX[t[k]];
        }
        for (int k = 0; k < 4; k++) rk[i * 4 + k] = rk[(i - 8) * 4 + k] ^ t[k];
    }
}

/* The state is column by column: byte c * 4 + row */
static void aes256_encrypt(const uint8_t rk[240], const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16], t[16];
    for (int i = 0; i < 16; i++) s[i] = in[i] ^ rk[i];
    for (int r = 1; r <= 14; r++) {
        for (int c = 0; c < 4; c++) {
            for (int row = 0; row < 4; row++) t[c * 4 + row] = SBOX[s[((c + row) % 4) * 4 + row]];
        }
        if (r != 14) {
            for (int c = 0; c < 4; c++) {
                uint8_t a0 = t[c * 4], a1 = t[c * 4 + 1], a2 = t[c * 4 + 2], a3 = t[c * 4 + 3];
                uint8_t all = a0 ^ a1 ^ a2 ^ a3;
                t[c * 4] = a0 ^ all ^ xtime(a0 ^ a1);
                t[c * 4 + 1] = a1 ^ all ^ xtime(a1 ^ a2);
                t[c * 4 + 2] = a2 ^ all ^ xtime(a2 ^ a3);
                t[c * 4 + 3] = a3 ^ all ^ xtime(a3 ^ a0);
            }
        }
        for (int i = 0; i < 16; i++) s[i] = t[i] ^ rk[r * 16 + i];
    }
    memcpy(out, s, 16);
    mini_wipe(s, sizeof(s));
    mini_wipe(t, sizeof(t));
}
#endif

/* ========================================================================= */
/* GCM (NIST SP 800-38D): 96-bit nonce, 128-bit tag                           */
/* ========================================================================= */

#if defined(ESP_PLATFORM)
/* PSA Crypto: its ESP driver runs GCM on the AES accelerator */
void mini_gcm_init(MiniGcm *g, const uint8_t key[32]) {
    g->key = PSA_KEY_ID_NULL;
    if (psa_crypto_init() != PSA_SUCCESS) return;
    psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&a, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&a, 256);
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&a, PSA_ALG_GCM);
    if (psa_import_key(&a, key, 32, &g->key) != PSA_SUCCESS) g->key = PSA_KEY_ID_NULL;
}

void mini_gcm_free(MiniGcm *g) {
    if (g->key != PSA_KEY_ID_NULL) psa_destroy_key(g->key);
    g->key = PSA_KEY_ID_NULL;
}

void mini_gcm_seal(MiniGcm *g, const uint8_t nonce[12], const uint8_t *aad, size_t aad_len, uint8_t *data, size_t n) {
    size_t out = 0;
    if (g->key == PSA_KEY_ID_NULL ||
        psa_aead_encrypt(g->key, PSA_ALG_GCM, nonce, 12, aad, aad_len, data, n, data, n + 16, &out) != PSA_SUCCESS) {
        memset(data, 0, n + 16);   /* nothing readable goes out; the other side rejects the tag */
    }
}

bool mini_gcm_open(MiniGcm *g, const uint8_t nonce[12], const uint8_t *aad, size_t aad_len, uint8_t *data, size_t n) {
    size_t out = 0;
    return g->key != PSA_KEY_ID_NULL &&
           psa_aead_decrypt(g->key, PSA_ALG_GCM, nonce, 12, aad, aad_len, data, n + 16, data, n, &out) == PSA_SUCCESS &&
           out == n;
}
#else
static void block_encrypt(MiniGcm *g, const uint8_t in[16], uint8_t out[16]) {
    aes256_encrypt(g->rk, in, out);
}

/* x = x * h in GF(2^128); constant time (masks, no branch on the bits) */
static void ghash_mul(uint8_t x[16], const uint8_t h[16]) {
    uint32_t v0 = load_be32(h), v1 = load_be32(h + 4), v2 = load_be32(h + 8), v3 = load_be32(h + 12);
    uint32_t z0 = 0, z1 = 0, z2 = 0, z3 = 0;
    for (int i = 0; i < 16; i++) {
        uint8_t b = x[i];
        for (int j = 7; j >= 0; j--) {
            uint32_t m = 0u - (uint32_t)((b >> j) & 1);
            z0 ^= v0 & m; z1 ^= v1 & m; z2 ^= v2 & m; z3 ^= v3 & m;
            uint32_t lsb = 0u - (v3 & 1);
            v3 = (v3 >> 1) | (v2 << 31);
            v2 = (v2 >> 1) | (v1 << 31);
            v1 = (v1 >> 1) | (v0 << 31);
            v0 = (v0 >> 1) ^ (0xE1000000u & lsb);
        }
    }
    store_be32(x, z0);
    store_be32(x + 4, z1);
    store_be32(x + 8, z2);
    store_be32(x + 12, z3);
}

static void ghash_update(const uint8_t h[16], uint8_t x[16], const uint8_t *p, size_t n) {
    while (n > 0) {
        size_t k = n < 16 ? n : 16;
        for (size_t i = 0; i < k; i++) x[i] ^= p[i];
        ghash_mul(x, h);
        p += k;
        n -= k;
    }
}

void mini_gcm_init(MiniGcm *g, const uint8_t key[32]) {
    aes256_expand(g->rk, key);
    static const uint8_t zero[16] = {0};
    block_encrypt(g, zero, g->h);
}

void mini_gcm_free(MiniGcm *g) {
    mini_wipe(g, sizeof(*g));
}

/* CTR from counter 2 (counter 1 masks the tag) */
static void gcm_ctr(MiniGcm *g, const uint8_t nonce[12], uint8_t *data, size_t n) {
    uint8_t cb[16], ks[16];
    memcpy(cb, nonce, 12);
    uint32_t ctr = 2;
    while (n > 0) {
        store_be32(cb + 12, ctr++);
        block_encrypt(g, cb, ks);
        size_t k = n < 16 ? n : 16;
        for (size_t i = 0; i < k; i++) data[i] ^= ks[i];
        data += k;
        n -= k;
    }
    mini_wipe(ks, sizeof(ks));
}

static void gcm_tag(MiniGcm *g, const uint8_t nonce[12], const uint8_t *aad, size_t aad_len,
                    const uint8_t *ct, size_t n, uint8_t tag[16]) {
    uint8_t x[16] = {0}, lens[16], j0[16], s[16];
    ghash_update(g->h, x, aad, aad_len);
    ghash_update(g->h, x, ct, n);
    uint64_t abits = (uint64_t)aad_len * 8, cbits = (uint64_t)n * 8;
    for (int i = 0; i < 8; i++) {
        lens[i] = (uint8_t)(abits >> (56 - 8 * i));
        lens[8 + i] = (uint8_t)(cbits >> (56 - 8 * i));
    }
    ghash_update(g->h, x, lens, 16);
    memcpy(j0, nonce, 12);
    store_be32(j0 + 12, 1);
    block_encrypt(g, j0, s);
    for (int i = 0; i < 16; i++) tag[i] = s[i] ^ x[i];
}

void mini_gcm_seal(MiniGcm *g, const uint8_t nonce[12], const uint8_t *aad, size_t aad_len, uint8_t *data, size_t n) {
    gcm_ctr(g, nonce, data, n);
    gcm_tag(g, nonce, aad, aad_len, data, n, data + n);
}

bool mini_gcm_open(MiniGcm *g, const uint8_t nonce[12], const uint8_t *aad, size_t aad_len, uint8_t *data, size_t n) {
    uint8_t want[16];
    gcm_tag(g, nonce, aad, aad_len, data, n, want);
    uint8_t diff = 0;
    for (int i = 0; i < 16; i++) diff |= (uint8_t)(want[i] ^ data[n + i]);
    if (diff != 0) return false;
    gcm_ctr(g, nonce, data, n);
    return true;
}
#endif
