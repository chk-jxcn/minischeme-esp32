/**
 * @file minischeme_channel.h
 * @brief The encrypted channel to a protocol converter (tools/app_proxy.py):
 *        AES-256-GCM with a key per device, over HTTP or plain TCP, instead
 *        of HTTPS (TLS needs 30-45 KB of RAM per connection on the board).
 *
 * Keys: the converter keeps one master key; a device's key is
 * HKDF-SHA256(salt "msc1 device key", ikm master, info device id), so the
 * converter needs no list of devices and the master key never leaves it.
 * Each request draws 16 random bytes (its nonce); HKDF-SHA256(salt nonce,
 * ikm device key, info "msc1 request keys") gives 64 bytes: the request key
 * and the response key. A key is used for one request only.
 *
 * Request (device -> converter), integers big-endian:
 *   "MSC1" | u8 id length | id | nonce (16) | u32 n | ciphertext (n) | tag (16)
 *   AAD: everything before the ciphertext; GCM nonce: 12 zero bytes.
 *   Plaintext: u32 unix time | u8 m | method (m) | u16 u | url (u) | body
 *   The url is absolute: the converter answers it itself (its own /app/...,
 *   /market/...) or fetches it (an internet http(s) URL).
 * Response (converter -> device): records until one flagged final
 *   u16 n (<= 1024) | u8 flags (1: final) | ciphertext (n) | tag (16)
 *   AAD: the 3 header bytes; GCM nonce: 4 zero bytes | u64 record number.
 *   The first record's plaintext starts with u16 status | u32 body length
 *   (0xFFFFFFFF: unknown); the rest of the records is the body.
 * The converter refuses a request older or newer than 5 minutes (the
 * device's clock: NTP) and a nonce it has seen; a stream cut short or a
 * record that fails its tag is an error, never a short document.
 *
 * Transports: http://host:port posts the request to /channel and reads the
 * records as the reply's body; tcp://host:port sends the request on a TCP
 * connection and reads the records until the converter closes it.
 */

#ifndef MINISCHEME_CHANNEL_H
#define MINISCHEME_CHANNEL_H

#include "minischeme_crypto.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MINI_CHAN_RECORD     1024   /* largest record plaintext */
#define MINI_CHAN_ID_MAX     64
#define MINI_CHAN_LENGTH_UNKNOWN 0xFFFFFFFFu

#define MINI_CHAN_ECLOSED  (-1)     /* the stream ended before the final record */
#define MINI_CHAN_EAUTH    (-2)     /* a record failed its tag */
#define MINI_CHAN_EIO      (-3)     /* the transport failed */
#define MINI_CHAN_EFORMAT  (-4)     /* a malformed record */

/** @brief Reads up to n bytes: > 0 bytes read, 0 at the end, < 0 an error. */
typedef int (*MiniChanRecv)(void *io, uint8_t *buf, int n);

/** The response side of one request */
typedef struct {
    MiniGcm gcm;
    uint64_t seq;            /* the next record's number */
    bool done;               /* the final record was read */
    int status;              /* from the first record */
    uint32_t total;          /* body length, MINI_CHAN_LENGTH_UNKNOWN */
    size_t len, pos;         /* plaintext in rec, handed out up to pos */
    uint8_t rec[MINI_CHAN_RECORD + 16];
} MiniChanReader;

/** @brief The request message for method url (malloc'd, *out_len bytes);
 *         reader is set up for its response. NULL: out of memory or an
 *         id / url too long. */
uint8_t *mini_chan_request(const uint8_t key[32], const char *id, const uint8_t nonce[16], uint32_t now,
                           const char *method, const char *url, const uint8_t *body, size_t body_len,
                           MiniChanReader *reader, size_t *out_len);
/** @brief Reads the first record (status and length); 0 or an error. */
int mini_chan_start(MiniChanReader *r, MiniChanRecv recv, void *io);
/** @brief Body bytes into buf: > 0, 0 at the verified end, < 0 an error. */
int mini_chan_read(MiniChanReader *r, MiniChanRecv recv, void *io, uint8_t *buf, int cap);
void mini_chan_reader_free(MiniChanReader *r);
const char *mini_chan_error(int code);
/** @brief 64 hex digits -> 32 bytes. */
bool mini_chan_parse_key(const char *hex, uint8_t key[32]);

/** @brief The device's channel settings (config.ini [channel]): server
 *         http://host:port or tcp://host:port, id, key (64 hex digits); the
 *         app proxy's and the market's base URLs and whether each goes
 *         through the converter. Empty or invalid: no channel. */
void mini_chan_configure(const char *server, const char *id, const char *key_hex,
                         const char *proxy_base, bool proxy_via, const char *market_base, bool market_via);
/** @brief A server, an id and a valid key are set. */
bool mini_chan_enabled(void);
/** @brief url goes through the converter: the app proxy's URLs and any
 *         https:// URL (the converter fetches it) with proxy_via, the
 *         market's with market_via. */
bool mini_chan_routes(const char *url);
/** @brief The converter address, "" without a channel. */
const char *mini_chan_server(void);

#if defined(ESP_PLATFORM)
/* One GET, plain HTTP or through the converter (mini_chan_routes) */
typedef struct MiniFetch MiniFetch;
#define MINI_FETCH_REDIRECTS 1      /* follow HTTP redirects (plain HTTP only) */
#define MINI_FETCH_BIG       2      /* 1 KB receive buffer instead of 512 B */
/** @brief Opens url: *status (HTTP status), *length (-1 unknown); NULL with
 *         err set when it cannot. */
MiniFetch *mini_fetch_open(const char *url, int timeout_ms, int flags, int *status, int64_t *length,
                           char *err, size_t err_cap);
/** @brief > 0 bytes, 0 at the end, < 0 an error. */
int mini_fetch_read(MiniFetch *f, char *buf, int cap);
/** @brief Whether the fetch goes through the converter. */
bool mini_fetch_encrypted(const MiniFetch *f);
void mini_fetch_close(MiniFetch *f);
#endif

#endif /* MINISCHEME_CHANNEL_H */
