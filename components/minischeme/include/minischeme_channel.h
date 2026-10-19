/**
 * @file minischeme_channel.h
 * @brief The encrypted channel to a protocol converter (tools/app_proxy.py):
 *        AES-256-GCM with a key per device, over HTTP or plain TCP, instead
 *        of HTTPS (TLS needs 30-45 KB of RAM per connection on the board).
 *
 * Keys: the converter keeps one master key; a device's key is
 * HKDF-SHA256(salt hardware hash key, ikm master, info device id), made when
 * the device registers (below) and kept in the converter's device list.
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

/* Server addresses in config.ini carry the device's key for that server:
 * "proxy = <key>@http://host:port" ("[<key>]@..." too), the key 64 hex
 * digits. A value without a key is a server the device has not registered
 * with yet. */
/** @brief The URL part of a config value (the value itself without a key). */
const char *mini_chan_url_of(const char *value);
/** @brief The key part of a "key@url" value; false without one. */
bool mini_chan_key_of(const char *value, uint8_t key[32]);
/** @brief scheme://host[:port] of url, lowercase, into out. */
void mini_chan_origin(const char *url, char *out, size_t cap);

#if defined(ESP_PLATFORM)
/* Servers and registration. The device id is its WiFi MAC (12 uppercase hex
 * digits); its hardware hash key is HKDF-SHA256(salt id, ikm the hardware
 * password, info "msc1_hw_hash"), the password itself never leaves it. The
 * first request to a server without a key registers there (POST
 * <server>/channel/auth): a new device gets its key, a known one answers a
 * challenge with HMAC-SHA256(hash key, challenge) and gets its key back.
 * The key is handed to the saver (written next to the address in its file).
 * A server without the channel, a device without a password or a failed
 * registration is used over plain HTTP. */
typedef enum { MINI_CHAN_PENDING, MINI_CHAN_READY, MINI_CHAN_PLAIN, MINI_CHAN_FAILED } MiniChanState;
/** @brief Called from the fetching task with a key to keep. */
typedef void (*MiniChanSaver)(const char *origin, const char *key_hex);

/** @brief The device id and hardware password ("" none); servers not ready
 *         are tried again. */
void mini_chan_set_device(const char *id, const char *password);
/** @brief The server list is rebuilt from config values between begin and
 *         end: a server keeps its state (and a key it registered) while a
 *         value names it; one no value names is dropped. */
void mini_chan_servers_begin(void);
void mini_chan_server_add(const char *value);
void mini_chan_servers_end(void);
/** @brief The converter for https:// URLs (a config value, key allowed). */
void mini_chan_set_https(const char *value);
void mini_chan_set_saver(MiniChanSaver fn);
/** @brief What a fetch waits for now: MINI_CHAN_STEP_CLOCK (the network
 *         time, the converter checks it) or MINI_CHAN_STEP_REGISTER (a
 *         registration), with the server into server; MINI_CHAN_STEP_NONE. */
#define MINI_CHAN_STEP_NONE     0
#define MINI_CHAN_STEP_CLOCK    1
#define MINI_CHAN_STEP_REGISTER 2
int mini_chan_step(char *server, size_t cap);
/** @brief Registers with every server not tried yet (blocks, the HTTP task). */
void mini_chan_register_all(int timeout_ms);
/** @brief The servers' states as a JSON array: server, state (ready, plain,
 *         failed, pending), reason (no_password, no_channel, password,
 *         unreachable, refused, tcp, error) and error. */
size_t mini_chan_status_json(char *out, size_t cap);

/* One GET, plain HTTP or through the converter: an https:// URL goes to the
 * https converter, any other to its own server when that server is ready */
typedef struct MiniFetch MiniFetch;
#define MINI_FETCH_REDIRECTS 1      /* follow HTTP redirects (plain HTTP only) */
#define MINI_FETCH_BIG       2      /* 1 KB receive buffer instead of 512 B */
#define MINI_FETCH_WAIT      (-2)   /* *status of a NULL open: not ready yet (the clock), try again */
/** @brief Opens url: *status (HTTP status), *length (-1 unknown); NULL with
 *         err set when it cannot (*status MINI_FETCH_WAIT: try again soon). */
MiniFetch *mini_fetch_open(const char *url, int timeout_ms, int flags, int *status, int64_t *length,
                           char *err, size_t err_cap);
/** @brief > 0 bytes, 0 at the end, < 0 an error. */
int mini_fetch_read(MiniFetch *f, char *buf, int cap);
/** @brief Whether the fetch goes through the converter. */
bool mini_fetch_encrypted(const MiniFetch *f);
void mini_fetch_close(MiniFetch *f);
#endif

#endif /* MINISCHEME_CHANNEL_H */
