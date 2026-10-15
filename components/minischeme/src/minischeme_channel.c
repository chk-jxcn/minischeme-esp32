/**
 * @file minischeme_channel.c
 * @brief The encrypted channel to the protocol converter (format in
 *        minischeme_channel.h); on the ESP32 also the GET used by the JSON
 *        feed and the app market, plain or through the converter.
 */

#include "minischeme_channel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(ESP_PLATFORM)
#include "esp_http_client.h"
#include "esp_random.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#endif

static const char REQUEST_INFO[] = "msc1 request keys";

static void put_be16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* ========================================================================= */
/* Messages                                                                   */
/* ========================================================================= */

uint8_t *mini_chan_request(const uint8_t key[32], const char *id, const uint8_t nonce[16], uint32_t now,
                           const char *method, const char *url, const uint8_t *body, size_t body_len,
                           MiniChanReader *reader, size_t *out_len) {
    size_t id_len = id ? strlen(id) : 0, m_len = strlen(method), u_len = strlen(url);
    if (id_len == 0 || id_len > MINI_CHAN_ID_MAX || m_len == 0 || m_len > 16 || u_len > 0xFFFF) return NULL;
    size_t head = 4 + 1 + id_len + 16 + 4;
    size_t pt_len = 4 + 1 + m_len + 2 + u_len + body_len;
    uint8_t *msg = (uint8_t *)malloc(head + pt_len + 16);
    if (!msg) return NULL;
    memcpy(msg, "MSC1", 4);
    msg[4] = (uint8_t)id_len;
    memcpy(msg + 5, id, id_len);
    memcpy(msg + 5 + id_len, nonce, 16);
    put_be32(msg + head - 4, (uint32_t)pt_len);
    uint8_t *p = msg + head;
    put_be32(p, now);
    p += 4;
    *p++ = (uint8_t)m_len;
    memcpy(p, method, m_len);
    p += m_len;
    put_be16(p, (uint16_t)u_len);
    p += 2;
    memcpy(p, url, u_len);
    p += u_len;
    if (body_len) memcpy(p, body, body_len);

    uint8_t okm[64];   /* the request key, then the response key */
    mini_hkdf_sha256(nonce, 16, key, 32, (const uint8_t *)REQUEST_INFO, sizeof(REQUEST_INFO) - 1, okm, sizeof(okm));
    static const uint8_t zero_nonce[12] = {0};
    MiniGcm g;
    mini_gcm_init(&g, okm);
    mini_gcm_seal(&g, zero_nonce, msg, head, msg + head, pt_len);   /* the tag follows the ciphertext */
    mini_gcm_free(&g);
    memset(reader, 0, sizeof(*reader));
    mini_gcm_init(&reader->gcm, okm + 32);
    reader->total = MINI_CHAN_LENGTH_UNKNOWN;
    mini_wipe(okm, sizeof(okm));
    *out_len = head + pt_len + 16;
    return msg;
}

static int recv_exact(MiniChanRecv recv, void *io, uint8_t *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        int k = recv(io, buf + got, (int)(n - got));
        if (k < 0) return MINI_CHAN_EIO;
        if (k == 0) return MINI_CHAN_ECLOSED;
        got += (size_t)k;
    }
    return 0;
}

/* The next record, opened into rec */
static int next_record(MiniChanReader *r, MiniChanRecv recv, void *io) {
    uint8_t hdr[3];
    int e = recv_exact(recv, io, hdr, sizeof(hdr));
    if (e) return e;
    size_t n = (size_t)hdr[0] << 8 | hdr[1];
    if (n > MINI_CHAN_RECORD) return MINI_CHAN_EFORMAT;
    e = recv_exact(recv, io, r->rec, n + 16);
    if (e) return e;
    uint8_t nonce[12] = {0};
    for (int i = 0; i < 8; i++) nonce[4 + i] = (uint8_t)(r->seq >> (56 - 8 * i));
    if (!mini_gcm_open(&r->gcm, nonce, hdr, sizeof(hdr), r->rec, n)) return MINI_CHAN_EAUTH;
    r->seq++;
    if (hdr[2] & 1) r->done = true;
    r->len = n;
    r->pos = 0;
    return 0;
}

int mini_chan_start(MiniChanReader *r, MiniChanRecv recv, void *io) {
    int e = next_record(r, recv, io);
    if (e) return e;
    if (r->len < 6) return MINI_CHAN_EFORMAT;
    r->status = r->rec[0] << 8 | r->rec[1];
    r->total = (uint32_t)r->rec[2] << 24 | (uint32_t)r->rec[3] << 16 | (uint32_t)r->rec[4] << 8 | r->rec[5];
    r->pos = 6;
    return 0;
}

int mini_chan_read(MiniChanReader *r, MiniChanRecv recv, void *io, uint8_t *buf, int cap) {
    for (;;) {
        if (r->pos < r->len) {
            size_t k = r->len - r->pos < (size_t)cap ? r->len - r->pos : (size_t)cap;
            memcpy(buf, r->rec + r->pos, k);
            r->pos += k;
            return (int)k;
        }
        if (r->done) return 0;
        int e = next_record(r, recv, io);
        if (e) return e;
    }
}

void mini_chan_reader_free(MiniChanReader *r) {
    if (!r) return;
    mini_gcm_free(&r->gcm);
    mini_wipe(r, sizeof(*r));
}

const char *mini_chan_error(int code) {
    switch (code) {
        case MINI_CHAN_ECLOSED: return "the converter closed the connection early";
        case MINI_CHAN_EAUTH:   return "a response record failed authentication";
        case MINI_CHAN_EIO:     return "the connection failed";
        case MINI_CHAN_EFORMAT: return "a malformed response record";
        default:                return "channel error";
    }
}

bool mini_chan_parse_key(const char *hex, uint8_t key[32]) {
    if (!hex || strlen(hex) != 64) return false;
    for (int i = 0; i < 64; i++) {
        char c = hex[i];
        int v = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10
              : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (v < 0) return false;
        if (i % 2 == 0) key[i / 2] = (uint8_t)(v << 4);
        else key[i / 2] |= (uint8_t)v;
    }
    return true;
}

/* ========================================================================= */
/* Settings and routing                                                       */
/* ========================================================================= */

typedef struct {
    char server[128];
    char id[MINI_CHAN_ID_MAX + 1];
    uint8_t key[32];
    char proxy[128];
    char market[160];
    bool proxy_via, market_via;
} ChanConfig;

static ChanConfig *s_chan = NULL;   /* allocated only when a channel is set */

static void copy_base(char *dst, size_t cap, const char *src) {
    snprintf(dst, cap, "%s", src ? src : "");
    size_t n = strlen(dst);
    while (n > 0 && dst[n - 1] == '/') dst[--n] = '\0';
}

void mini_chan_configure(const char *server, const char *id, const char *key_hex,
                         const char *proxy_base, bool proxy_via, const char *market_base, bool market_via) {
    if (s_chan) {
        mini_wipe(s_chan, sizeof(*s_chan));
        free(s_chan);
        s_chan = NULL;
    }
    uint8_t key[32];
    bool scheme_ok = server && (strncmp(server, "http://", 7) == 0 || strncmp(server, "tcp://", 6) == 0);
    size_t id_len = id ? strlen(id) : 0;
    if (!scheme_ok || id_len == 0 || id_len > MINI_CHAN_ID_MAX || !mini_chan_parse_key(key_hex, key)) return;
    s_chan = (ChanConfig *)calloc(1, sizeof(ChanConfig));
    if (s_chan) {
        copy_base(s_chan->server, sizeof(s_chan->server), server);
        snprintf(s_chan->id, sizeof(s_chan->id), "%s", id);
        memcpy(s_chan->key, key, sizeof(key));
        copy_base(s_chan->proxy, sizeof(s_chan->proxy), proxy_base);
        copy_base(s_chan->market, sizeof(s_chan->market), market_base);
        s_chan->proxy_via = proxy_via;
        s_chan->market_via = market_via;
    }
    mini_wipe(key, sizeof(key));
}

bool mini_chan_enabled(void) { return s_chan != NULL; }

const char *mini_chan_server(void) { return s_chan ? s_chan->server : ""; }

/* url is base or below it (base "http://a:80" does not take "http://a:8087") */
static bool url_under(const char *url, const char *base) {
    size_t n = strlen(base);
    if (n == 0 || strncmp(url, base, n) != 0) return false;
    return url[n] == '\0' || url[n] == '/' || url[n] == '?';
}

bool mini_chan_routes(const char *url) {
    if (!s_chan || !url) return false;
    if (s_chan->proxy_via && (url_under(url, s_chan->proxy) || strncmp(url, "https://", 8) == 0)) return true;
    return s_chan->market_via && url_under(url, s_chan->market);
}

/* ========================================================================= */
/* ESP32: one GET, plain or through the converter                             */
/* ========================================================================= */

#if defined(ESP_PLATFORM)
struct MiniFetch {
    esp_http_client_handle_t h;   /* plain HTTP, or the channel's HTTP transport */
    int sock;                     /* the channel's TCP transport, else -1 */
    MiniChanReader *r;            /* through the converter */
};

static int http_recv(void *io, uint8_t *buf, int n) {
    int k = esp_http_client_read(((MiniFetch *)io)->h, (char *)buf, n);
    return k < 0 ? -1 : k;
}

static int tcp_recv(void *io, uint8_t *buf, int n) {
    int k = (int)recv(((MiniFetch *)io)->sock, buf, (size_t)n, 0);
    return k < 0 ? -1 : k;
}

/* "host:port" connected within timeout_ms; -1 with err set */
static int tcp_connect(const char *hostport, int timeout_ms, char *err, size_t cap) {
    char host[96], port[8];
    const char *colon = strrchr(hostport, ':');
    if (!colon || colon == hostport || (size_t)(colon - hostport) >= sizeof(host) || strlen(colon + 1) >= sizeof(port)) {
        snprintf(err, cap, "converter: expected tcp://host:port");
        return -1;
    }
    memcpy(host, hostport, (size_t)(colon - hostport));
    host[colon - hostport] = '\0';
    snprintf(port, sizeof(port), "%s", colon + 1);
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res) {
        snprintf(err, cap, "converter: cannot resolve %.40s", host);
        return -1;
    }
    int s = socket(res->ai_family, res->ai_socktype, 0);
    bool ok = s >= 0;
    if (ok) {
        /* connect without waiting longer than the timeout */
        int fl = fcntl(s, F_GETFL, 0);
        fcntl(s, F_SETFL, fl | O_NONBLOCK);
        int r = connect(s, res->ai_addr, res->ai_addrlen);
        if (r < 0 && errno == EINPROGRESS) {
            fd_set wr;
            FD_ZERO(&wr);
            FD_SET(s, &wr);
            struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
            int so = 0;
            socklen_t sl = sizeof(so);
            r = (select(s + 1, NULL, &wr, NULL, &tv) == 1 && getsockopt(s, SOL_SOCKET, SO_ERROR, &so, &sl) == 0 &&
                 so == 0) ? 0 : -1;
        }
        fcntl(s, F_SETFL, fl);
        struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        ok = r == 0;
    }
    freeaddrinfo(res);
    if (!ok) {
        if (s >= 0) close(s);
        snprintf(err, cap, "converter: cannot connect");
        return -1;
    }
    return s;
}

static bool send_all(int s, const uint8_t *p, size_t n) {
    while (n > 0) {
        int k = (int)send(s, p, n, 0);
        if (k <= 0) return false;
        p += k;
        n -= (size_t)k;
    }
    return true;
}

/* The request sent over HTTP (POST <server>/channel); the reply's status */
static bool chan_http_send(MiniFetch *f, const uint8_t *msg, size_t len, int timeout_ms, char *err, size_t cap) {
    size_t n = strlen(s_chan->server) + 9;
    char *url = (char *)malloc(n);
    if (!url) {
        snprintf(err, cap, "out of memory");
        return false;
    }
    snprintf(url, n, "%s/channel", s_chan->server);
    esp_http_client_config_t cfg = {
        .url = url, .method = HTTP_METHOD_POST, .timeout_ms = timeout_ms,
        .buffer_size = 512, .buffer_size_tx = 256, .disable_auto_redirect = true,
    };
    f->h = esp_http_client_init(&cfg);
    free(url);
    if (!f->h) {
        snprintf(err, cap, "out of memory");
        return false;
    }
    esp_http_client_set_header(f->h, "Content-Type", "application/octet-stream");
    if (esp_http_client_open(f->h, (int)len) != ESP_OK) {
        snprintf(err, cap, "converter: cannot connect");
        return false;
    }
    if (esp_http_client_write(f->h, (const char *)msg, (int)len) != (int)len) {
        snprintf(err, cap, "converter: send failed");
        return false;
    }
    esp_http_client_fetch_headers(f->h);
    int status = esp_http_client_get_status_code(f->h);
    if (status != 200) {
        snprintf(err, cap, status == 403 ? "converter: refused (check channel_id and channel_key)"
                                         : "converter: HTTP %d", status);
        return false;
    }
    return true;
}

MiniFetch *mini_fetch_open(const char *url, int timeout_ms, int flags, int *status, int64_t *length,
                           char *err, size_t err_cap) {
    MiniFetch *f = (MiniFetch *)calloc(1, sizeof(MiniFetch));
    if (!f) {
        snprintf(err, err_cap, "out of memory");
        return NULL;
    }
    f->sock = -1;
    *status = 0;
    *length = -1;
    if (!mini_chan_routes(url)) {
        bool big = (flags & MINI_FETCH_BIG) != 0;
        esp_http_client_config_t cfg = {
            .url = url, .timeout_ms = timeout_ms, .buffer_size = big ? 1024 : 512, .buffer_size_tx = big ? 512 : 256,
            .disable_auto_redirect = !(flags & MINI_FETCH_REDIRECTS), .max_redirection_count = 3,
        };
        f->h = esp_http_client_init(&cfg);
        if (!f->h || esp_http_client_open(f->h, 0) != ESP_OK) {
            snprintf(err, err_cap, f->h ? "cannot connect" : "out of memory");
            goto fail;
        }
        int64_t n = esp_http_client_fetch_headers(f->h);
        *status = esp_http_client_get_status_code(f->h);
        *length = n > 0 ? n : -1;
        return f;
    }

    /* through the converter */
    time_t now = time(NULL);
    if (now < 1600000000) {   /* the converter checks the time: wait for NTP */
        snprintf(err, err_cap, "converter: the clock is not set yet");
        goto fail;
    }
    f->r = (MiniChanReader *)malloc(sizeof(MiniChanReader));
    uint8_t nonce[16];
    esp_fill_random(nonce, sizeof(nonce));
    size_t len = 0;
    uint8_t *msg = f->r ? mini_chan_request(s_chan->key, s_chan->id, nonce, (uint32_t)now, "GET", url, NULL, 0,
                                            f->r, &len) : NULL;
    if (!msg) {
        free(f->r);
        f->r = NULL;
        snprintf(err, err_cap, "out of memory");
        goto fail;
    }
    bool sent;
    MiniChanRecv recv_fn;
    if (strncmp(s_chan->server, "tcp://", 6) == 0) {
        f->sock = tcp_connect(s_chan->server + 6, timeout_ms, err, err_cap);
        sent = f->sock >= 0 && send_all(f->sock, msg, len);
        if (f->sock >= 0 && !sent) snprintf(err, err_cap, "converter: send failed");
        recv_fn = tcp_recv;
    } else {
        sent = chan_http_send(f, msg, len, timeout_ms, err, err_cap);
        recv_fn = http_recv;
    }
    mini_wipe(msg, len);
    free(msg);
    if (!sent) goto fail;
    int e = mini_chan_start(f->r, recv_fn, f);
    if (e) {
        snprintf(err, err_cap, e == MINI_CHAN_ECLOSED ? "converter: refused (check channel_id, channel_key, clock)"
                                                      : "converter: %s", mini_chan_error(e));
        goto fail;
    }
    if (f->r->status != 200) {
        /* the converter's reason (the clock, the upstream) in a short text body */
        char why[48];
        int n = mini_chan_read(f->r, recv_fn, f, (uint8_t *)why, (int)sizeof(why) - 1);
        why[n > 0 ? n : 0] = '\0';
        snprintf(err, err_cap, "converter: %d %s", f->r->status, why);
        goto fail;
    }
    *status = f->r->status;
    *length = f->r->total == MINI_CHAN_LENGTH_UNKNOWN ? -1 : (int64_t)f->r->total;
    return f;
fail:
    mini_fetch_close(f);
    return NULL;
}

int mini_fetch_read(MiniFetch *f, char *buf, int cap) {
    if (f->r) return mini_chan_read(f->r, f->sock >= 0 ? tcp_recv : http_recv, f, (uint8_t *)buf, cap);
    return esp_http_client_read(f->h, buf, cap);
}

bool mini_fetch_encrypted(const MiniFetch *f) { return f && f->r != NULL; }

void mini_fetch_close(MiniFetch *f) {
    if (!f) return;
    if (f->h) {
        esp_http_client_close(f->h);
        esp_http_client_cleanup(f->h);
    }
    if (f->sock >= 0) close(f->sock);
    if (f->r) {
        mini_chan_reader_free(f->r);
        free(f->r);
    }
    free(f);
}
#endif
