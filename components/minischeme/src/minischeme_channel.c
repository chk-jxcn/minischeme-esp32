/**
 * @file minischeme_channel.c
 * @brief The encrypted channel to the protocol converter (format in
 *        minischeme_channel.h); on the ESP32 also the servers the device
 *        registers with and the GET used by the JSON feed and the app
 *        market, plain or through the converter.
 */

#include "minischeme_channel.h"
#include "minischeme_lib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(ESP_PLATFORM)
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
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
/* "key@url" config values                                                    */
/* ========================================================================= */

static bool is_hex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* The key's 64 digits in value, else NULL; *url_at: the URL after it */
static const char *key_part(const char *value, const char **url_at) {
    const char *k = value;
    bool bracket = *k == '[';
    if (bracket) k++;
    for (int i = 0; i < 64; i++) {
        if (!is_hex(k[i])) return NULL;
    }
    const char *p = k + 64;
    if (bracket && *p++ != ']') return NULL;
    if (*p != '@') return NULL;
    *url_at = p + 1;
    return k;
}

const char *mini_chan_url_of(const char *value) {
    const char *url;
    if (!value) return "";
    while (*value == ' ' || *value == '\t') value++;
    return key_part(value, &url) ? url : value;
}

bool mini_chan_key_of(const char *value, uint8_t key[32]) {
    const char *url, *k;
    if (!value) return false;
    while (*value == ' ' || *value == '\t') value++;
    k = key_part(value, &url);
    if (!k) return false;
    char hex[65];
    memcpy(hex, k, 64);
    hex[64] = '\0';
    bool ok = mini_chan_parse_key(hex, key);
    mini_wipe(hex, sizeof(hex));
    return ok;
}

void mini_chan_origin(const char *url, char *out, size_t cap) {
    if (cap == 0) return;
    out[0] = '\0';
    const char *s = url ? strstr(url, "://") : NULL;
    if (!s) return;
    size_t n = (size_t)(s + 3 - url);
    while (url[n] && url[n] != '/' && url[n] != '?' && url[n] != '#') n++;
    if (n >= cap) n = cap - 1;
    for (size_t i = 0; i < n; i++) {
        char c = url[i];
        out[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    out[n] = '\0';
}

/* ========================================================================= */
/* ESP32: servers, registration, and one GET plain or through the converter   */
/* ========================================================================= */

#if defined(ESP_PLATFORM)
#define CHAN_SERVERS 4
#define CHAN_RETRY_US (30LL * 1000000)   /* an unreachable server is tried again after this */

typedef struct {
    char origin[96];
    uint8_t key[32];
    bool used, seen, has_key;
    bool from_config;    /* this round: a config value carried the key */
    bool plain_entry;    /* this round: a value named the server without a key */
    bool retried;        /* registered again once after the server refused the key */
    uint8_t state;       /* MiniChanState */
    char reason[16];
    char error[80];
    int64_t retry_us;    /* PENDING: not before this time */
} ChanServer;

static ChanServer s_srv[CHAN_SERVERS];
static char s_dev_id[MINI_CHAN_ID_MAX + 1];
static uint8_t s_hash_key[32];
static bool s_has_pass;
static char s_https[128];
static MiniChanSaver s_saver;
static SemaphoreHandle_t s_lock, s_reg_lock;   /* the table; one registration at a time */
static int s_step = MINI_CHAN_STEP_NONE;        /* what a fetch waits for now (mini_chan_step) */
static char s_step_server[96];

static void chan_lock(void) {
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void chan_unlock(void) {
    if (s_lock) xSemaphoreGive(s_lock);
}

static void set_step(int step, const char *server) {
    chan_lock();
    s_step = step;
    snprintf(s_step_server, sizeof(s_step_server), "%s", server ? server : "");
    chan_unlock();
}

/* The step ends unless another one (a registration by another task) replaced it */
static void end_step(int step) {
    chan_lock();
    if (s_step == step) {
        s_step = MINI_CHAN_STEP_NONE;
        s_step_server[0] = '\0';
    }
    chan_unlock();
}

int mini_chan_step(char *server, size_t cap) {
    chan_lock();
    int step = s_step;
    if (cap) snprintf(server, cap, "%s", s_step_server);
    chan_unlock();
    return step;
}

static ChanServer *find_server(const char *origin) {
    for (int i = 0; i < CHAN_SERVERS; i++) {
        if (s_srv[i].used && strcmp(s_srv[i].origin, origin) == 0) return &s_srv[i];
    }
    return NULL;
}

static void set_state(ChanServer *s, MiniChanState st, const char *reason, const char *error) {
    s->state = (uint8_t)st;
    snprintf(s->reason, sizeof(s->reason), "%s", reason);
    snprintf(s->error, sizeof(s->error), "%s", error);
}

static void to_hex(const uint8_t *b, size_t n, char *out) {
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = d[b[i] >> 4];
        out[2 * i + 1] = d[b[i] & 15];
    }
    out[2 * n] = '\0';
}

void mini_chan_set_device(const char *id, const char *password) {
    chan_lock();
    if (!s_reg_lock) s_reg_lock = xSemaphoreCreateMutex();
    snprintf(s_dev_id, sizeof(s_dev_id), "%s", id ? id : "");
    s_has_pass = password && password[0] && s_dev_id[0];
    if (s_has_pass) {
        static const char info[] = "msc1_hw_hash";
        mini_hkdf_sha256((const uint8_t *)s_dev_id, strlen(s_dev_id), (const uint8_t *)password, strlen(password),
                         (const uint8_t *)info, sizeof(info) - 1, s_hash_key, sizeof(s_hash_key));
    } else {
        mini_wipe(s_hash_key, sizeof(s_hash_key));
    }
    for (int i = 0; i < CHAN_SERVERS; i++) {
        if (s_srv[i].used && s_srv[i].state != MINI_CHAN_READY) {
            set_state(&s_srv[i], MINI_CHAN_PENDING, "", "");
            s_srv[i].retry_us = 0;
        }
    }
    chan_unlock();
}

void mini_chan_set_saver(MiniChanSaver fn) { s_saver = fn; }

void mini_chan_servers_begin(void) {
    chan_lock();
    for (int i = 0; i < CHAN_SERVERS; i++) {
        s_srv[i].seen = s_srv[i].from_config = s_srv[i].plain_entry = false;
    }
    chan_unlock();
}

void mini_chan_server_add(const char *value) {
    char origin[96];
    mini_chan_origin(mini_chan_url_of(value), origin, sizeof(origin));
    if (strncmp(origin, "http://", 7) != 0 && strncmp(origin, "tcp://", 6) != 0) return;
    if (origin[strlen(origin) - 1] == '/') return;   /* no host */
    uint8_t key[32];
    bool keyed = mini_chan_key_of(value, key);
    chan_lock();
    ChanServer *s = find_server(origin);
    for (int i = 0; !s && i < CHAN_SERVERS; i++) {
        if (!s_srv[i].used) {
            s = &s_srv[i];
            memset(s, 0, sizeof(*s));
            s->used = true;
            snprintf(s->origin, sizeof(s->origin), "%s", origin);
            set_state(s, MINI_CHAN_PENDING, "", "");
        }
    }
    if (s) {
        s->seen = true;
        if (keyed && !s->from_config) {   /* the file's key comes first */
            bool same = s->has_key && memcmp(s->key, key, sizeof(key)) == 0;
            memcpy(s->key, key, sizeof(key));
            s->has_key = s->from_config = true;
            if (!same || s->state != MINI_CHAN_FAILED) set_state(s, MINI_CHAN_READY, "", "");
        } else if (!keyed) {
            s->plain_entry = true;
        }
    }
    chan_unlock();
    mini_wipe(key, sizeof(key));
}

void mini_chan_servers_end(void) {
    char save[CHAN_SERVERS][96], hex[CHAN_SERVERS][65];
    int n = 0;
    chan_lock();
    for (int i = 0; i < CHAN_SERVERS; i++) {
        ChanServer *s = &s_srv[i];
        if (!s->used) continue;
        if (!s->seen) {
            mini_wipe(s, sizeof(*s));
            continue;
        }
        if (s->has_key && s->state == MINI_CHAN_READY && s->plain_entry) {   /* a value lost the key: write it again */
            memcpy(save[n], s->origin, sizeof(save[n]));   /* both 96 bytes, NUL-terminated */
            to_hex(s->key, sizeof(s->key), hex[n]);
            n++;
        }
    }
    chan_unlock();
    for (int i = 0; i < n; i++) {
        if (s_saver) s_saver(save[i], hex[i]);
        mini_wipe(hex[i], sizeof(hex[i]));
    }
}

void mini_chan_set_https(const char *value) {
    chan_lock();
    snprintf(s_https, sizeof(s_https), "%s", mini_chan_url_of(value));
    size_t n = strlen(s_https);
    while (n > 0 && s_https[n - 1] == '/') s_https[--n] = '\0';
    chan_unlock();
}

/* ---- registration (POST <server>/channel/auth) ---- */

/* A JSON POST: the status (-1: no answer) and up to cap - 1 bytes of the body */
static int post_json(const char *url, const char *body, char *resp, size_t cap, int timeout_ms) {
    resp[0] = '\0';
    esp_http_client_config_t cfg = {
        .url = url, .method = HTTP_METHOD_POST, .timeout_ms = timeout_ms,
        .buffer_size = 512, .buffer_size_tx = 512, .disable_auto_redirect = true,
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) return -1;
    int status = -1, n = (int)strlen(body);
    esp_http_client_set_header(h, "Content-Type", "application/json");
    if (esp_http_client_open(h, n) == ESP_OK && esp_http_client_write(h, body, n) == n) {
        esp_http_client_fetch_headers(h);
        status = esp_http_client_get_status_code(h);
        int got = 0, k;
        while (got < (int)cap - 1 && (k = esp_http_client_read(h, resp + got, (int)cap - 1 - got)) > 0) got += k;
        resp[got] = '\0';
    }
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    return status;
}

/* The string value of "name" in a flat JSON object */
static bool json_str(const char *json, const char *name, char *out, size_t cap) {
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\"", name);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p == ' ') p++;
    if (*p++ != ':') return false;
    while (*p == ' ') p++;
    if (*p++ != '"') return false;
    size_t n = 0;
    while (p[n] && p[n] != '"' && n + 1 < cap) n++;
    memcpy(out, p, n);
    out[n] = '\0';
    return p[n] == '"';
}

static size_t parse_hex(const char *hex, uint8_t *out, size_t cap) {
    size_t n = 0;
    for (; is_hex(hex[2 * n]) && is_hex(hex[2 * n + 1]) && n < cap; n++) {
        char two[3] = { hex[2 * n], hex[2 * n + 1], '\0' };
        out[n] = (uint8_t)strtoul(two, NULL, 16);
    }
    return n;
}

typedef struct {
    MiniChanState state;
    const char *reason;
    char error[80];
    int64_t retry_us;
    uint8_t key[32];
} RegResult;

static void reg_unreachable(RegResult *r) {
    r->state = MINI_CHAN_PENDING;
    r->reason = "unreachable";
    snprintf(r->error, sizeof(r->error), "cannot reach the server; trying again later");
    r->retry_us = esp_timer_get_time() + CHAN_RETRY_US;
}

static bool reg_key(RegResult *r, const char *resp) {
    char hex[72];
    if (!json_str(resp, "master_key_2", hex, sizeof(hex)) || !mini_chan_parse_key(hex, r->key)) {
        r->state = MINI_CHAN_FAILED;
        r->reason = "error";
        snprintf(r->error, sizeof(r->error), "the server sent no key");
        return false;
    }
    mini_wipe(hex, sizeof(hex));
    r->state = MINI_CHAN_READY;
    r->reason = "";
    return true;
}

/* The buffers of one registration (on the heap: the JSON feed's task has 4 KB of stack) */
typedef struct {
    char url[112], body[200], resp[400], hex[65], status[24], challenge[136];
    uint8_t ch[64], mac[32];
} RegBufs;

static void reg_exchange(const char *origin, const char *id, const uint8_t hash_key[32], int timeout_ms,
                         RegResult *r) {
    RegBufs *b = (RegBufs *)calloc(1, sizeof(RegBufs));
    if (!b) {
        reg_unreachable(r);
        snprintf(r->error, sizeof(r->error), "out of memory; trying again later");
        return;
    }
    snprintf(b->url, sizeof(b->url), "%s/channel/auth", origin);
    to_hex(hash_key, 32, b->hex);
    snprintf(b->body, sizeof(b->body), "{\"mac\":\"%s\",\"hardware_hash_key\":\"%s\"}", id, b->hex);
    int st = post_json(b->url, b->body, b->resp, sizeof(b->resp), timeout_ms);
    if (st < 0 || st >= 500) {
        reg_unreachable(r);
    } else if (st == 404 || (st == 403 && strstr(b->resp, "disabled"))) {
        r->state = MINI_CHAN_PLAIN;
        r->reason = "no_channel";
        snprintf(r->error, sizeof(r->error), "the server has no encrypted channel: plain HTTP");
    } else if (st == 200 && json_str(b->resp, "status", b->status, sizeof(b->status)) &&
               strcmp(b->status, "registered") == 0) {
        reg_key(r, b->resp);
    } else if (st == 200 && strcmp(b->status, "challenge_required") == 0 &&
               json_str(b->resp, "challenge", b->challenge, sizeof(b->challenge))) {
        /* registered before (a factory reset): prove the hash key, get the key back */
        size_t n = parse_hex(b->challenge, b->ch, sizeof(b->ch));
        mini_hmac_sha256(hash_key, 32, b->ch, n, b->mac);
        to_hex(b->mac, sizeof(b->mac), b->hex);
        snprintf(b->body, sizeof(b->body), "{\"mac\":\"%s\",\"response\":\"%s\"}", id, b->hex);
        st = post_json(b->url, b->body, b->resp, sizeof(b->resp), timeout_ms);
        if (st < 0 || st >= 500) {
            reg_unreachable(r);
        } else if (st == 200) {
            reg_key(r, b->resp);
        } else {
            r->state = MINI_CHAN_FAILED;
            r->reason = st == 403 ? "password" : "error";
            snprintf(r->error, sizeof(r->error), st == 403
                     ? "the hardware password differs from the one this device registered with"
                     : "registration: HTTP %d", st);
        }
    } else {
        r->state = MINI_CHAN_FAILED;
        r->reason = st == 403 ? "password" : "error";
        snprintf(r->error, sizeof(r->error), st == 403 ? "the server refused the registration"
                                                       : "registration: HTTP %d", st);
    }
    mini_wipe(b, sizeof(*b));
    free(b);
}

/* Registers with the server at origin when it waits for it; true with its key when ready */
static bool chan_ready(const char *origin, int timeout_ms, uint8_t key[32]) {
    chan_lock();
    ChanServer *s = find_server(origin);
    bool ready = s && s->state == MINI_CHAN_READY;
    bool due = s && s->state == MINI_CHAN_PENDING && esp_timer_get_time() >= s->retry_us;
    if (ready) memcpy(key, s->key, 32);
    chan_unlock();
    if (ready || !due) return ready;
    if (!s_reg_lock || xSemaphoreTake(s_reg_lock, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return false;
    RegResult r;
    memset(&r, 0, sizeof(r));
    uint8_t hash_key[32];
    char id[MINI_CHAN_ID_MAX + 1];
    chan_lock();
    s = find_server(origin);
    ready = s && s->state == MINI_CHAN_READY;   /* another task registered meanwhile */
    due = s && s->state == MINI_CHAN_PENDING && esp_timer_get_time() >= s->retry_us;
    if (ready) memcpy(key, s->key, 32);
    bool has_pass = s_has_pass;
    memcpy(hash_key, s_hash_key, sizeof(hash_key));
    snprintf(id, sizeof(id), "%s", s_dev_id);
    chan_unlock();
    if (due) {
        if (!has_pass) {
            r.state = MINI_CHAN_PLAIN;
            r.reason = "no_password";
            snprintf(r.error, sizeof(r.error), "no hardware password ([server] hardware_password): plain HTTP");
        } else if (strncmp(origin, "tcp://", 6) == 0) {
            r.state = MINI_CHAN_FAILED;
            r.reason = "tcp";
            snprintf(r.error, sizeof(r.error), "a tcp:// converter needs a key: register over http:// first");
        } else {
            set_step(MINI_CHAN_STEP_REGISTER, origin);
            reg_exchange(origin, id, hash_key, timeout_ms < 4000 ? 4000 : timeout_ms, &r);
            end_step(MINI_CHAN_STEP_REGISTER);
        }
        chan_lock();
        s = find_server(origin);
        if (s) {
            set_state(s, r.state, r.reason, r.error);
            s->retry_us = r.retry_us;
            if (r.state == MINI_CHAN_READY) {
                memcpy(s->key, r.key, 32);
                s->has_key = true;
            }
        }
        chan_unlock();
        ready = r.state == MINI_CHAN_READY;
        if (ready) {
            char hex[65];
            memcpy(key, r.key, 32);
            to_hex(r.key, 32, hex);
            if (s_saver) s_saver(origin, hex);
            mini_wipe(hex, sizeof(hex));
        }
        ESP_LOGI("MiniChannel", "%s: %s%s%s", origin, ready ? "registered" : "not registered",
                 r.error[0] ? ": " : "", r.error);
    }
    xSemaphoreGive(s_reg_lock);
    mini_wipe(hash_key, sizeof(hash_key));
    mini_wipe(&r, sizeof(r));
    return ready;
}

/* The server refused the device's key: register again once, then give up */
static void chan_refused(const char *origin) {
    chan_lock();
    ChanServer *s = find_server(origin);
    if (s && s->state == MINI_CHAN_READY) {
        mini_wipe(s->key, sizeof(s->key));
        s->has_key = false;
        if (!s->retried) {
            s->retried = true;
            s->retry_us = 0;
            set_state(s, MINI_CHAN_PENDING, "refused", "the server refused the key: registering again");
        } else {
            set_state(s, MINI_CHAN_FAILED, "refused", "the server refuses this device's key");
        }
    }
    chan_unlock();
}

void mini_chan_register_all(int timeout_ms) {
    for (int i = 0; i < CHAN_SERVERS; i++) {
        char origin[96] = "";
        uint8_t key[32];
        chan_lock();
        if (s_srv[i].used && s_srv[i].state == MINI_CHAN_PENDING) {
            memcpy(origin, s_srv[i].origin, sizeof(origin));
        }
        chan_unlock();
        if (origin[0]) chan_ready(origin, timeout_ms, key);
        mini_wipe(key, sizeof(key));
    }
}

size_t mini_chan_status_json(char *out, size_t cap) {
    static const char *const names[] = { "pending", "ready", "plain", "failed" };
    size_t n = 0;
    if (cap < 3) return 0;
    out[n++] = '[';
    chan_lock();
    for (int i = 0; i < CHAN_SERVERS; i++) {
        const ChanServer *s = &s_srv[i];
        if (!s->used || n + 2 >= cap) continue;
        int k = snprintf(out + n, cap - n, "%s{\"server\":\"%s\",\"state\":\"%s\",\"reason\":\"%s\",\"error\":\"%s\"}",
                         n > 1 ? "," : "", s->origin, names[s->state & 3], s->reason, s->error);
        if (k < 0 || (size_t)k >= cap - n - 1) break;
        n += (size_t)k;
    }
    chan_unlock();
    out[n++] = ']';
    out[n] = '\0';
    return n;
}

/* ---- one GET ---- */

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

/* The request sent over HTTP (POST <server>/channel); *refused: a 403 */
static bool chan_http_send(MiniFetch *f, const char *server, const uint8_t *msg, size_t len, int timeout_ms,
                           bool *refused, char *err, size_t cap) {
    char url[112];
    snprintf(url, sizeof(url), "%s/channel", server);
    esp_http_client_config_t cfg = {
        .url = url, .method = HTTP_METHOD_POST, .timeout_ms = timeout_ms,
        .buffer_size = 512, .buffer_size_tx = 256, .disable_auto_redirect = true,
    };
    f->h = esp_http_client_init(&cfg);
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
        *refused = status == 403;
        snprintf(err, cap, status == 403 ? "converter: refused the device's key (registering again)"
                                         : "converter: HTTP %d", status);
        return false;
    }
    return true;
}

/* <base>/fetch?url=<url, percent-encoded> (malloc'd) */
static char *fetch_url(const char *base, const char *url) {
    static const char hex[] = "0123456789ABCDEF";
    size_t cap = strlen(base) + 12 + strlen(url) * 3 + 1;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    size_t n = (size_t)snprintf(out, cap, "%s/fetch?url=", base);
    for (const unsigned char *p = (const unsigned char *)url; *p; p++) {
        bool plain = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
                     *p == '-' || *p == '_' || *p == '.' || *p == '~';
        if (plain) {
            out[n++] = (char)*p;
        } else {
            out[n++] = '%';
            out[n++] = hex[*p >> 4];
            out[n++] = hex[*p & 15];
        }
    }
    out[n] = '\0';
    return out;
}

MiniFetch *mini_fetch_open(const char *url, int timeout_ms, int flags, int *status, int64_t *length,
                           char *err, size_t err_cap) {
    MiniFetch *f = (MiniFetch *)calloc(1, sizeof(MiniFetch));
    char *plain_url = NULL;
    if (!f) {
        snprintf(err, err_cap, "out of memory");
        return NULL;
    }
    f->sock = -1;
    *status = 0;
    *length = -1;
    /* the server: an https:// URL's converter, else the URL's own */
    bool https = strncmp(url, "https://", 8) == 0;
    char base[128], origin[96];
    chan_lock();
    snprintf(base, sizeof(base), "%s", s_https);
    chan_unlock();
    if (https && !base[0]) {
        snprintf(err, err_cap, "an https:// URL needs a protocol converter: config.ini [app] https_proxy or proxy");
        goto fail;
    }
    mini_chan_origin(https ? base : url, origin, sizeof(origin));
    uint8_t key[32];
    if (!chan_ready(origin, timeout_ms, key)) {
        if (https) {
            plain_url = fetch_url(base, url);
            if (!plain_url) {
                snprintf(err, err_cap, "out of memory");
                goto fail;
            }
        }
        bool big = (flags & MINI_FETCH_BIG) != 0;
        esp_http_client_config_t cfg = {
            .url = plain_url ? plain_url : url, .timeout_ms = timeout_ms,
            .buffer_size = big ? 1024 : 512, .buffer_size_tx = big ? 512 : 256,
            .disable_auto_redirect = !(flags & MINI_FETCH_REDIRECTS), .max_redirection_count = 3,
        };
        f->h = esp_http_client_init(&cfg);
        free(plain_url);
        if (!f->h || esp_http_client_open(f->h, 0) != ESP_OK) {
            snprintf(err, err_cap, f->h ? "cannot connect" : "out of memory");
            goto fail;
        }
        int64_t n = esp_http_client_fetch_headers(f->h);
        *status = esp_http_client_get_status_code(f->h);
        *length = n > 0 ? n : -1;
        return f;
    }

    /* through the converter: it checks the time, so the clock waits for NTP
     * (up to the timeout, then MINI_FETCH_WAIT: not an error, try again) */
    time_t now = time(NULL);
    if (now < 1600000000) {
        /* without WiFi the footer says so, not "waiting for the time" */
        int step = MINI_CHAN_STEP_NONE;
        for (int waited = 0; (now = time(NULL)) < 1600000000 && waited < timeout_ms; waited += 250) {
            int want = esp32_wifi_connected() ? MINI_CHAN_STEP_CLOCK : MINI_CHAN_STEP_WIFI;
            if (want != step) set_step(step = want, origin);
            vTaskDelay(pdMS_TO_TICKS(250));
        }
        if (now < 1600000000) {
            mini_wipe(key, sizeof(key));
            *status = MINI_FETCH_WAIT;
            snprintf(err, err_cap, step == MINI_CHAN_STEP_WIFI ? "waiting for WiFi" : "waiting for the network time (NTP)");
            goto fail;
        }
        end_step(step);
    }
    f->r = (MiniChanReader *)malloc(sizeof(MiniChanReader));
    uint8_t nonce[16];
    esp_fill_random(nonce, sizeof(nonce));
    size_t len = 0;
    uint8_t *msg = f->r ? mini_chan_request(key, s_dev_id, nonce, (uint32_t)now, "GET", url, NULL, 0,
                                            f->r, &len) : NULL;
    mini_wipe(key, sizeof(key));
    if (!msg) {
        free(f->r);
        f->r = NULL;
        snprintf(err, err_cap, "out of memory");
        goto fail;
    }
    bool sent, refused = false;
    MiniChanRecv recv_fn;
    if (strncmp(origin, "tcp://", 6) == 0) {
        f->sock = tcp_connect(origin + 6, timeout_ms, err, err_cap);
        sent = f->sock >= 0 && send_all(f->sock, msg, len);
        if (f->sock >= 0 && !sent) snprintf(err, err_cap, "converter: send failed");
        recv_fn = tcp_recv;
    } else {
        sent = chan_http_send(f, origin, msg, len, timeout_ms, &refused, err, err_cap);
        recv_fn = http_recv;
    }
    mini_wipe(msg, len);
    free(msg);
    if (!sent) {
        if (refused) chan_refused(origin);
        goto fail;
    }
    int e = mini_chan_start(f->r, recv_fn, f);
    if (e) {
        if (e == MINI_CHAN_ECLOSED && f->sock >= 0) chan_refused(origin);   /* tcp: a refusal closes */
        snprintf(err, err_cap, e == MINI_CHAN_ECLOSED ? "converter: refused (the device's key or the clock)"
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
