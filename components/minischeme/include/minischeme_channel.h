/**
 * @file minischeme_channel.h
 * @brief The device's requests to app servers and protocol converters
 *        (minischeme-proxy app_proxy.py, server.py): plain HTTP (no TLS, which needs
 *        30-45 KB of RAM per connection on the board, and since firmware
 *        1.0.69 no encryption either: it saves its RAM and code).
 *
 * Every request to a server named in config.ini carries
 *   X-MiniScheme-Device    the device id (its WiFi MAC, 12 uppercase hex digits)
 *   X-MiniScheme-Password  base64 of the app service password set for that
 *                          server (<key>_password), when it has one
 * The server registers the device with the password at its first request
 * (keeping a hash of it) and refuses another password later (HTTP 403).
 * Plain HTTP shows both headers to anyone on the way: base64 is no
 * encryption.
 *
 * Config values written by older firmware carry a key for the encrypted
 * channel: "proxy = <key>@http://host:port" ("[<key>]@..." too, the key
 * 64 hex digits). The key is ignored: mini_chan_url_of gives the address.
 */

#ifndef MINISCHEME_CHANNEL_H
#define MINISCHEME_CHANNEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MINI_CHAN_ID_MAX     64
#define MINI_CHAN_PASSWORD_MAX 64   /* bytes of an app service password */

/** @brief Overwrites n bytes at p with zeros (a password no longer needed);
 *         the compiler may not drop it. */
static inline void mini_wipe(void *p, size_t n) {
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n--) *v++ = 0;
}

/** @brief The URL part of a config value (without an old channel key). */
const char *mini_chan_url_of(const char *value);
/** @brief Whether value starts with an old channel key ("<key>@url"). */
bool mini_chan_has_key(const char *value);
/** @brief scheme://host[:port] of url, lowercase, into out. */
void mini_chan_origin(const char *url, char *out, size_t cap);

#if defined(ESP_PLATFORM)
/** @brief The device id (its WiFi MAC) sent in X-MiniScheme-Device. */
void mini_chan_set_device(const char *id);
/** @brief The server list is rebuilt from config values between begin and
 *         end. password: the value's app service password (its
 *         <key>_password), "" for none; the first value naming a server
 *         with a password gives it. relay (an app server with
 *         <key>_relay = 1 and no password): its requests go through the
 *         system converter instead (<converter>/relay?url=). */
void mini_chan_servers_begin(void);
void mini_chan_server_add(const char *value, const char *password, bool relay);
void mini_chan_servers_end(void);
/** @brief The system converter app servers are relayed through (a config value). */
void mini_chan_set_relay(const char *value);
/** @brief The converter for https:// URLs (a config value). */
void mini_chan_set_https(const char *value);
/** @brief Tests a server and an app service password, registering nothing
 *         (the settings page's test button, GET <server>/device/check):
 *         "ok" (the device is registered there with it), "new" (unknown
 *         there: its first request registers it), "password" (registered
 *         with another one), "plain" (no password: the server answers),
 *         "old" (a server without /device/check: it predates firmware
 *         1.0.69), "unreachable", "error". detail: the reason. Blocks. */
const char *mini_chan_test(const char *url, const char *password, int timeout_ms, char *detail, size_t cap);
/** @brief The servers as a JSON array: server, auth (password, plain,
 *         relay) and the last request's outcome: state (unknown, ok,
 *         refused: a wrong password, failed) and error. */
size_t mini_chan_status_json(char *out, size_t cap);
/** @brief Kept for json-feed-status: always MINI_CHAN_STEP_NONE now (a
 *         request no longer waits for the clock or a registration). */
#define MINI_CHAN_STEP_NONE     0
int mini_chan_step(char *server, size_t cap);

/* One GET: an https:// URL through the https converter (/fetch?url=), an
 * app server set to be relayed through the system converter (/relay?url=),
 * any other to its own server */
typedef struct MiniFetch MiniFetch;
#define MINI_FETCH_REDIRECTS 1      /* follow HTTP redirects */
#define MINI_FETCH_BIG       2      /* 1 KB receive buffer instead of 512 B */
#define MINI_FETCH_WAIT      (-2)   /* kept for callers: no longer returned */
/** @brief Opens url: *status (HTTP status), *length (-1 unknown); NULL with
 *         err set when it cannot. */
MiniFetch *mini_fetch_open(const char *url, int timeout_ms, int flags, int *status, int64_t *length,
                           char *err, size_t err_cap);
/** @brief > 0 bytes, 0 at the end, < 0 an error. */
int mini_fetch_read(MiniFetch *f, char *buf, int cap);
void mini_fetch_close(MiniFetch *f);
#endif

#endif /* MINISCHEME_CHANNEL_H */
