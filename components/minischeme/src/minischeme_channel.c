/**
 * @file minischeme_channel.c
 * @brief The device's requests to app servers and converters over plain
 *        HTTP with its id and app service password in headers (see
 *        minischeme_channel.h); on the ESP32 also the servers it knows and
 *        the GET used by the JSON feed and the app market.
 */

#include "minischeme_channel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(ESP_PLATFORM)
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#endif

/* ========================================================================= */
/* Config values (an old channel key before the address is ignored)          */
/* ========================================================================= */

static bool is_hex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* value after "<64 hex digits>@" or "[<64 hex digits>]@", else NULL */
static const char *after_key(const char *value) {
    const char *k = value;
    bool bracket = *k == '[';
    if (bracket) k++;
    for (int i = 0; i < 64; i++) {
        if (!is_hex(k[i])) return NULL;
    }
    const char *p = k + 64;
    if (bracket && *p++ != ']') return NULL;
    return *p == '@' ? p + 1 : NULL;
}

const char *mini_chan_url_of(const char *value) {
    if (!value) return "";
    while (*value == ' ' || *value == '\t') value++;
    const char *url = after_key(value);
    return url ? url : value;
}

bool mini_chan_has_key(const char *value) {
    if (!value) return false;
    while (*value == ' ' || *value == '\t') value++;
    return after_key(value) != NULL;
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
/* ESP32: the servers and one GET                                             */
/* ========================================================================= */

#if defined(ESP_PLATFORM)
#define CHAN_SERVERS 4
#define B64_MAX (((MINI_CHAN_PASSWORD_MAX + 2) / 3) * 4 + 1)

enum { OUT_UNKNOWN, OUT_OK, OUT_REFUSED, OUT_FAILED };

typedef struct {
    char origin[96];
    char pw64[B64_MAX];  /* base64 of the app service password, "" for none */
    bool used, seen, relay;
    uint8_t outcome;     /* the last request's: OUT_* */
    char error[48];
} ChanServer;

static ChanServer s_srv[CHAN_SERVERS];
static char s_dev_id[MINI_CHAN_ID_MAX + 1];
static char s_https[128];
static char s_relay[128];   /* the system converter app servers are relayed through */
static SemaphoreHandle_t s_lock;

static void chan_lock(void) {
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void chan_unlock(void) {
    if (s_lock) xSemaphoreGive(s_lock);
}

static ChanServer *find_server(const char *origin) {
    for (int i = 0; i < CHAN_SERVERS; i++) {
        if (s_srv[i].used && strcmp(s_srv[i].origin, origin) == 0) return &s_srv[i];
    }
    return NULL;
}

/* base64 of n bytes into out (cap >= 4 * ceil(n / 3) + 1) */
static void b64(const uint8_t *p, size_t n, char *out) {
    static const char d[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)p[i] << 16 | (i + 1 < n ? (uint32_t)p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        out[o++] = d[(v >> 18) & 63];
        out[o++] = d[(v >> 12) & 63];
        out[o++] = i + 1 < n ? d[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? d[v & 63] : '=';
    }
    out[o] = '\0';
}

void mini_chan_set_device(const char *id) {
    chan_lock();
    snprintf(s_dev_id, sizeof(s_dev_id), "%s", id ? id : "");
    chan_unlock();
}

void mini_chan_servers_begin(void) {
    chan_lock();
    for (int i = 0; i < CHAN_SERVERS; i++) {
        s_srv[i].seen = s_srv[i].relay = false;
        mini_wipe(s_srv[i].pw64, sizeof(s_srv[i].pw64));   /* named again with its password this round */
    }
    chan_unlock();
}

void mini_chan_server_add(const char *value, const char *password, bool relay) {
    char origin[96], pw64[B64_MAX] = "";
    mini_chan_origin(mini_chan_url_of(value), origin, sizeof(origin));
    if (strncmp(origin, "http://", 7) != 0 || origin[strlen(origin) - 1] == '/') return;   /* no host */
    size_t pn = password ? strlen(password) : 0;
    if (pn > MINI_CHAN_PASSWORD_MAX) pn = MINI_CHAN_PASSWORD_MAX;
    if (pn) b64((const uint8_t *)password, pn, pw64);
    chan_lock();
    ChanServer *s = find_server(origin);
    for (int i = 0; !s && i < CHAN_SERVERS; i++) {
        if (!s_srv[i].used) {
            s = &s_srv[i];
            memset(s, 0, sizeof(*s));
            s->used = true;
            snprintf(s->origin, sizeof(s->origin), "%s", origin);
        }
    }
    if (s) {
        s->seen = true;
        if (pw64[0] && !s->pw64[0]) {     /* the first value naming the server with a password */
            memcpy(s->pw64, pw64, sizeof(pw64));
            s->outcome = OUT_UNKNOWN;
        }
        if (relay && !s->pw64[0]) s->relay = true;
    }
    chan_unlock();
    mini_wipe(pw64, sizeof(pw64));
}

void mini_chan_servers_end(void) {
    chan_lock();
    for (int i = 0; i < CHAN_SERVERS; i++) {
        if (s_srv[i].used && !s_srv[i].seen) mini_wipe(&s_srv[i], sizeof(s_srv[i]));
        if (s_srv[i].pw64[0]) s_srv[i].relay = false;   /* a password: its own server */
    }
    chan_unlock();
}

static void set_base(char *dst, size_t cap, const char *value) {
    chan_lock();
    snprintf(dst, cap, "%s", mini_chan_url_of(value));
    size_t n = strlen(dst);
    while (n > 0 && dst[n - 1] == '/') dst[--n] = '\0';
    chan_unlock();
}

void mini_chan_set_relay(const char *value) { set_base(s_relay, sizeof(s_relay), value); }
void mini_chan_set_https(const char *value) { set_base(s_https, sizeof(s_https), value); }

int mini_chan_step(char *server, size_t cap) {
    if (cap) server[0] = '\0';
    return MINI_CHAN_STEP_NONE;
}

/* The headers of a request to origin: the device id, and the password of
 * that server when it has one */
static void put_headers(esp_http_client_handle_t h, const char *origin) {
    char id[MINI_CHAN_ID_MAX + 1], pw64[B64_MAX];
    chan_lock();
    ChanServer *s = find_server(origin);
    snprintf(id, sizeof(id), "%s", s_dev_id);
    snprintf(pw64, sizeof(pw64), "%s", s ? s->pw64 : "");
    chan_unlock();
    if (id[0]) esp_http_client_set_header(h, "X-MiniScheme-Device", id);
    if (pw64[0]) esp_http_client_set_header(h, "X-MiniScheme-Password", pw64);
    mini_wipe(pw64, sizeof(pw64));
}

/* The last request's outcome at origin (the settings page shows it) */
static void note(const char *origin, uint8_t outcome, const char *error) {
    chan_lock();
    ChanServer *s = find_server(origin);
    if (s) {
        s->outcome = outcome;
        snprintf(s->error, sizeof(s->error), "%s", error ? error : "");
    }
    chan_unlock();
}

/* The value of "name": "..." in a small JSON text into out (spaces allowed
 * around the colon; no escapes expected) */
static bool json_str(const char *json, const char *name, char *out, size_t cap) {
    char pat[24];
    snprintf(pat, sizeof(pat), "\"%s\"", name);
    const char *p = strstr(json, pat);
    if (!p || cap == 0) return false;
    p += strlen(pat);
    while (*p == ' ') p++;
    if (*p++ != ':') return false;
    while (*p == ' ') p++;
    if (*p++ != '"') return false;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < cap) out[n++] = *p++;
    out[n] = '\0';
    return true;
}

const char *mini_chan_test(const char *url, const char *password, int timeout_ms, char *detail, size_t cap) {
    char origin[96], target[112], id[MINI_CHAN_ID_MAX + 1], pw64[B64_MAX] = "", body[384];
    detail[0] = '\0';
    mini_chan_origin(mini_chan_url_of(url), origin, sizeof(origin));
    if (strncmp(origin, "http://", 7) != 0 || origin[strlen(origin) - 1] == '/') {
        snprintf(detail, cap, "an http://host:port address is needed");
        return "error";
    }
    size_t pn = password ? strlen(password) : 0;
    if (pn > MINI_CHAN_PASSWORD_MAX) {
        snprintf(detail, cap, "the password has more than %d bytes", MINI_CHAN_PASSWORD_MAX);
        return "error";
    }
    if (pn) b64((const uint8_t *)password, pn, pw64);
    chan_lock();
    snprintf(id, sizeof(id), "%s", s_dev_id);
    chan_unlock();
    snprintf(target, sizeof(target), "%s/device/check", origin);
    esp_http_client_config_t cfg = {
        .url = target, .timeout_ms = timeout_ms, .buffer_size = 512, .buffer_size_tx = 512,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) {
        mini_wipe(pw64, sizeof(pw64));
        snprintf(detail, cap, "out of memory");
        return "error";
    }
    if (id[0]) esp_http_client_set_header(h, "X-MiniScheme-Device", id);
    if (pw64[0]) esp_http_client_set_header(h, "X-MiniScheme-Password", pw64);
    mini_wipe(pw64, sizeof(pw64));
    int st = -1, n = 0;
    if (esp_http_client_open(h, 0) == ESP_OK) {
        esp_http_client_fetch_headers(h);
        st = esp_http_client_get_status_code(h);
        int k;
        while (n < (int)sizeof(body) - 1 && (k = esp_http_client_read(h, body + n, (int)sizeof(body) - 1 - n)) > 0) n += k;
    }
    body[n] = '\0';
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    if (st < 0) {
        snprintf(detail, cap, "cannot connect to %s", origin);
        return "unreachable";
    }
    if (st == 404) {
        snprintf(detail, cap, "%s answered, but has no /device/check: update the server", origin);
        return "old";
    }
    char status[16], why[96];
    if (st != 200 || !json_str(body, "status", status, sizeof(status))) {
        snprintf(detail, cap, "%s answered HTTP %d", origin, st);
        return "error";
    }
    if (json_str(body, "detail", why, sizeof(why))) snprintf(detail, cap, "%s", why);
    static const char *const known[] = { "ok", "new", "password", "plain" };
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        if (strcmp(status, known[i]) == 0) return known[i];
    }
    return "error";
}

size_t mini_chan_status_json(char *out, size_t cap) {
    static const char *const outcomes[] = { "unknown", "ok", "refused", "failed" };
    size_t n = 0;
    if (cap < 3) return 0;
    out[n++] = '[';
    chan_lock();
    for (int i = 0; i < CHAN_SERVERS; i++) {
        const ChanServer *s = &s_srv[i];
        if (!s->used || n + 2 >= cap) continue;
        int k = snprintf(out + n, cap - n, "%s{\"server\":\"%s\",\"auth\":\"%s\",\"state\":\"%s\",\"error\":\"%s\"}",
                         n > 1 ? "," : "", s->origin, s->pw64[0] ? "password" : s->relay ? "relay" : "plain",
                         outcomes[s->outcome & 3], s->error);
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
    esp_http_client_handle_t h;
};

/* <base>/<path>?url=<url, percent-encoded> (malloc'd): path "fetch" (an
 * https:// URL the converter fetches) or "relay" (an app server reached
 * through the system converter, which passes the device id on) */
static char *fetch_url(const char *base, const char *path, const char *url) {
    static const char hex[] = "0123456789ABCDEF";
    size_t cap = strlen(base) + strlen(path) + 8 + strlen(url) * 3 + 1;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    size_t n = (size_t)snprintf(out, cap, "%s/%s?url=", base, path);
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
    *status = 0;
    *length = -1;
    /* the server: an https:// URL's converter; an app server without a
     * password that asks for it, the system converter (/relay); else the
     * URL's own */
    bool https = strncmp(url, "https://", 8) == 0;
    char base[128], origin[96];
    bool relay = false, known = false;
    chan_lock();
    snprintf(base, sizeof(base), "%s", s_https);
    if (!https) {
        mini_chan_origin(url, origin, sizeof(origin));
        ChanServer *own = find_server(origin);
        known = own != NULL;
        relay = own && own->relay && s_relay[0];
        if (relay) snprintf(base, sizeof(base), "%s", s_relay);
    }
    chan_unlock();
    if (https && !base[0]) {
        snprintf(err, err_cap, "an https:// URL needs a protocol converter: config.ini [app] https_proxy or proxy");
        return NULL;
    }
    char *via = (https || relay) ? fetch_url(base, https ? "fetch" : "relay", url) : NULL;
    MiniFetch *f = (MiniFetch *)calloc(1, sizeof(MiniFetch));
    if ((https || relay) && !via) {
        free(f);
        snprintf(err, err_cap, "out of memory");
        return NULL;
    }
    if (!f) {
        free(via);
        snprintf(err, err_cap, "out of memory");
        return NULL;
    }
    mini_chan_origin(via ? base : url, origin, sizeof(origin));
    bool big = (flags & MINI_FETCH_BIG) != 0;
    esp_http_client_config_t cfg = {
        .url = via ? via : url, .timeout_ms = timeout_ms,
        .buffer_size = big ? 1024 : 512, .buffer_size_tx = 512,   /* a request line of up to ~500 bytes */
        .disable_auto_redirect = !(flags & MINI_FETCH_REDIRECTS), .max_redirection_count = 3,
    };
    f->h = esp_http_client_init(&cfg);
    free(via);
    if (f->h && (known || https || relay)) put_headers(f->h, origin);   /* the server tells its devices apart */
    if (!f->h || esp_http_client_open(f->h, 0) != ESP_OK) {
        snprintf(err, err_cap, f->h ? "cannot connect" : "out of memory");
        note(origin, OUT_FAILED, err);
        mini_fetch_close(f);
        return NULL;
    }
    int64_t n = esp_http_client_fetch_headers(f->h);
    *status = esp_http_client_get_status_code(f->h);
    *length = n > 0 ? n : -1;
    if (*status == 403) note(origin, OUT_REFUSED, "HTTP 403: a wrong app service password?");
    else note(origin, OUT_OK, "");
    return f;
}

int mini_fetch_read(MiniFetch *f, char *buf, int cap) {
    return esp_http_client_read(f->h, buf, cap);
}

void mini_fetch_close(MiniFetch *f) {
    if (!f) return;
    if (f->h) {
        esp_http_client_close(f->h);
        esp_http_client_cleanup(f->h);
    }
    free(f);
}
#endif
