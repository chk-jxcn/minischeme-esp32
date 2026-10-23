/**
 * @file minischeme_jsonfeed.c
 * @brief JSON feed: background HTTP polling plus a streaming JSON object
 *        selector, driven entirely by Scheme. See minischeme_jsonfeed.h.
 *
 * Nothing here has a fixed size: keys, selectors, rows, value text, nesting
 * levels and the URL are allocated as a feed needs them. The only bound is
 * memory: on the ESP32 a buffer stops growing when the free heap would fall
 * under JF_RAM_RESERVE (the WiFi stack and the HTTP server need it), and the
 * document then reports `truncated`. An app may still set its own limits
 * (pool-bytes, value-len, depth of json-feed-start).
 */

#include "minischeme_jsonfeed.h"
#include "minischeme_device.h"
#include "minischeme_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(ESP_PLATFORM)
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "minischeme_channel.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
static int64_t jf_now_ms(void) { return esp_timer_get_time() / 1000; }
#elif defined(__wasm__)
extern double js_now_ms(void);
static int64_t jf_now_ms(void) { return (int64_t)js_now_ms(); }
#else
#include <time.h>
static int64_t jf_now_ms(void) { return (int64_t)time(NULL) * 1000; }
#endif

#define JF_NONE          0xFFFFFFFFu  /* no value: key absent, not kept */
#define JF_RETRY_MS      5000         /* next try after a failed fetch */
#define JF_RAM_RESERVE   (16 * 1024)  /* ESP32: free heap the feed never takes */

enum { JF_EQUAL, JF_PREFIX };
enum { JF_IDLE, JF_CONNECTING, JF_OK, JF_ERROR };

typedef struct {
    int       key;              /* index into keys */
    int       mode;
    size_t    len;
    char     *text;
    uint32_t *keep;             /* keys stored for matching objects (bit set), NULL: all */
} JfSelector;

typedef struct {
    char       *url;
    uint32_t    interval_ms;
    size_t      value_max;      /* bytes kept of a value, 0: no limit */
    int         depth_max;      /* objects captured up to this nesting level, 0: no limit */
    size_t      pool_max;       /* bytes of value text per document, 0: no limit */
    char      **keys;
    int         nkeys;
    size_t      key_maxlen;
    JfSelector *sel;
    int         nsel;
    uint32_t    generation;
} JfConfig;

typedef struct {
    int       count, cap;       /* rows kept, rows allocated */
    int       nkeys;            /* values per row */
    uint32_t *sel;              /* [cap]: the selector each row matched */
    uint32_t *off;              /* [cap * nkeys] into pool, JF_NONE: absent */
    char     *pool;
    size_t    used, pcap;
    bool      truncated;        /* rows or text did not fit */
    int       objects;          /* objects with a captured key */
    uint32_t  generation;
} JfSnap;

/* One nesting level of the scanner: the values captured for the object open
 * at that level (a new object at the level starts it over) */
typedef struct {
    uint32_t *off;              /* [nkeys] into buf, JF_NONE: not seen */
    int       nkeys;            /* off's size */
    int       have;             /* values captured */
    bool      ok;               /* buffers present: this level is captured */
    char     *buf;
    size_t    used, cap;
} JfLevel;

typedef struct {
    JfLevel *lv;                /* levels allocated so far (kept across documents) */
    int      nlv;
    int      depth;
    bool     in_str, in_scalar, esc, expect_value;
    int      uhex;
    unsigned ucode;
    unsigned uhigh;      /* a \uD800..\uDBFF waiting for its low surrogate, else 0 */
    int      key;               /* captured key index of the pending value, -1 none */
    bool     tok_keep;          /* this token is stored (a key, or a captured value) */
    size_t   tok_limit;         /* bytes of it stored */
    char    *tok;
    size_t   tok_len, tok_cap;
} JfParser;

/* Allocated on the first json-feed-start (or feed), so apps that never use a
 * feed pay no RAM for it. The fetcher fills `work`; (json-feed-seq) swaps it
 * with `view` on the VM thread, so Scheme reads one consistent document
 * without locking. */
typedef struct {
    JfConfig *cfg;              /* fetcher-owned */
    JfConfig *pending;          /* from json-feed-start, adopted by the next document */
    JfSnap    a, b;
    JfSnap   *work, *view;
    bool      ready;            /* work holds a complete document not yet taken */
    JfParser  p;
    uint32_t  bytes;
    char     *dump;             /* listing being built */
    size_t    dump_len, dump_cap;
} JfState;

static JfState *S = NULL;
static uint32_t s_seq;          /* snapshots taken by the VM */
static uint32_t s_latest_gen;   /* generation of the newest configuration */
static int      s_state = JF_IDLE;
static char     s_error[64];
static uint32_t s_fetches, s_failures, s_last_bytes, s_last_ms;
static int64_t  s_last_ok_ms;
static char    *s_dump_pub = NULL;
static size_t   s_dump_pub_len = 0;
static uint32_t s_dump_seq = 0;
static char    *s_https_proxy = NULL;

#if defined(ESP_PLATFORM)
static SemaphoreHandle_t s_lock = NULL;
static TaskHandle_t s_task = NULL;
static volatile bool s_want_dump = false;
/* Created on first use: a query (json-feed-url, -status) may come before
 * any json-feed-start (the settings, a fresh app) */
static void jf_lock(void) {
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    xSemaphoreTake(s_lock, portMAX_DELAY);
}
#define JF_LOCK()   jf_lock()
#define JF_UNLOCK() xSemaphoreGive(s_lock)
#else
static bool s_want_dump = true; /* WASM/host: keep a listing of every document */
#define JF_LOCK()   ((void)0)
#define JF_UNLOCK() ((void)0)
#endif

/* ========================================================================= */
/* Memory                                                                    */
/* ========================================================================= */

/* Whether `extra` more bytes leave the system enough (always on hosts) */
static bool jf_room(size_t extra) {
#if defined(ESP_PLATFORM)
    return heap_caps_get_free_size(MALLOC_CAP_8BIT) > extra + JF_RAM_RESERVE;
#else
    (void)extra;
    return true;
#endif
}

/* *buf holds *cap elements of `size` bytes; makes room for `need` (doubling).
 * False when memory is short: *buf is unchanged. */
static bool jf_reserve(void **buf, size_t *cap, size_t need, size_t size) {
    if (need <= *cap) return true;
    size_t n = *cap ? *cap * 2 : 16;
    if (n < need) n = need;
    if (!jf_room((n - *cap) * size)) {
        n = need;   /* a smaller step may still fit */
        if (!jf_room((n - *cap) * size)) return false;
    }
    void *b = realloc(*buf, n * size);
    if (!b) return false;
    *buf = b;
    *cap = n;
    return true;
}

/* As jf_reserve for word arrays (row selectors, value offsets): in word
 * memory (IRAM on the ESP32, which leaves the heap alone; the heap once it
 * is full); grown with a word copy, never realloc (IRAM is word-only) */
static bool jf_reserve_words(uint32_t **buf, size_t *cap, size_t need) {
    if (need <= *cap) return true;
    size_t n = *cap ? *cap * 2 : 16;
    if (n < need) n = need;
    uint32_t *p = (uint32_t *)mini_word_alloc(n * sizeof(uint32_t));
    if (!p) return false;
    mini_words_copy(p, *buf, *cap * sizeof(uint32_t));
    free(*buf);
    *buf = p;
    *cap = n;
    return true;
}

static char *jf_strdup(const char *s, size_t n) {
    char *d = (char *)malloc(n + 1);
    if (!d) return NULL;
    memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

static void config_free(JfConfig *c) {
    if (!c) return;
    free(c->url);
    for (int k = 0; k < c->nkeys; k++) free(c->keys[k]);
    free(c->keys);
    for (int s = 0; s < c->nsel; s++) {
        free(c->sel[s].text);
        free(c->sel[s].keep);
    }
    free(c->sel);
    free(c);
}

/* Bytes the feed holds now: buffers of both snapshots and the scanner */
static size_t jf_memory(void) {
    if (!S) return 0;
    size_t n = sizeof(JfState);
    const JfSnap *sn[2] = { &S->a, &S->b };
    for (int i = 0; i < 2; i++) {
        n += sn[i]->pcap + (size_t)sn[i]->cap * sizeof(uint32_t) * (1 + (size_t)sn[i]->nkeys);
    }
    for (int l = 0; l < S->p.nlv; l++) n += S->p.lv[l].cap + (size_t)S->p.lv[l].nkeys * sizeof(uint32_t);
    n += (size_t)S->p.nlv * sizeof(JfLevel) + S->p.tok_cap + S->dump_cap;
    return n;
}

/* UTF-8 text cut at n bytes, not inside a character */
static size_t utf8_cut(const char *s, size_t len, size_t n) {
    if (len <= n) return len;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
    return n;
}

/* n bytes of UTF-8 that were cut off there: without a last character that
 * lost its tail */
static size_t utf8_trim_tail(const char *s, size_t n) {
    size_t i = n;
    while (i > 0 && n - i < 4 && ((unsigned char)s[i - 1] & 0xC0) == 0x80) i--;
    if (i == 0) return n;
    unsigned char lead = (unsigned char)s[i - 1];
    size_t want = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    return (i - 1) + want > n ? i - 1 : n;
}

static void copy_str(char *dst, size_t cap, const char *src) {
    size_t n = utf8_cut(src, strlen(src), cap - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static bool jf_alloc(void) {
    if (S) return true;
    S = (JfState *)calloc(1, sizeof(JfState));
    if (!S) return false;
    S->work = &S->a;
    S->view = &S->b;
    return true;
}

uint32_t mini_json_feed_interval(void) {
    if (!S) return 0;
    const JfConfig *c = S->pending ? S->pending : S->cfg;
    return c && c->url[0] ? c->interval_ms : 0;
}

uint32_t mini_json_feed_generation(void) {
    return s_latest_gen;
}

const char *mini_json_feed_url(void) {
    if (!S) return "";
    if (S->pending) return S->pending->url;
    return S->cfg ? S->cfg->url : "";
}

/* ========================================================================= */
/* Streaming parser                                                          */
/* ========================================================================= */

static void tok_put(JfParser *p, char c) {
    if (!p->tok_keep || p->tok_len >= p->tok_limit) return;
    if (p->tok_len + 1 >= p->tok_cap &&
        !jf_reserve((void **)&p->tok, &p->tok_cap, p->tok_len + 2, 1)) {
        p->tok_limit = p->tok_len;    /* out of memory: the value is cut here */
        if (S) S->work->truncated = true;
        return;
    }
    p->tok[p->tok_len++] = c;
}

static void tok_put_utf8(JfParser *p, unsigned cp) {
    if (cp < 0x80) {
        tok_put(p, (char)cp);
    } else if (cp < 0x800) {
        tok_put(p, (char)(0xC0 | (cp >> 6)));
        tok_put(p, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        tok_put(p, (char)(0xE0 | (cp >> 12)));
        tok_put(p, (char)(0x80 | ((cp >> 6) & 0x3F)));
        tok_put(p, (char)(0x80 | (cp & 0x3F)));
    } else {
        tok_put(p, (char)(0xF0 | (cp >> 18)));
        tok_put(p, (char)(0x80 | ((cp >> 12) & 0x3F)));
        tok_put(p, (char)(0x80 | ((cp >> 6) & 0x3F)));
        tok_put(p, (char)(0x80 | (cp & 0x3F)));
    }
}

/* A high surrogate not followed by its low half: U+FFFD */
static void tok_flush_high(JfParser *p) {
    if (p->uhigh) {
        p->uhigh = 0;
        tok_put_utf8(p, 0xFFFD);
    }
}

/* A \uXXXX code: surrogate pairs make one character (emoji); an unpaired
 * surrogate becomes U+FFFD */
static void tok_put_escape(JfParser *p, unsigned code) {
    if (code >= 0xDC00 && code <= 0xDFFF && p->uhigh) {
        tok_put_utf8(p, 0x10000 + ((p->uhigh - 0xD800) << 10) + (code - 0xDC00));
        p->uhigh = 0;
        return;
    }
    if (p->uhigh) {
        tok_put_utf8(p, 0xFFFD);
        p->uhigh = 0;
    }
    if (code >= 0xD800 && code <= 0xDBFF) p->uhigh = code;
    else tok_put_utf8(p, (code >= 0xDC00 && code <= 0xDFFF) ? 0xFFFD : code);
}

static void dump_append(const char *s, size_t n) {
    if (!S->dump) return;
    if (!jf_reserve((void **)&S->dump, &S->dump_cap, S->dump_len + n + 1, 1)) return;
    memcpy(S->dump + S->dump_len, s, n);
    S->dump_len += n;
    S->dump[S->dump_len] = '\0';
}

/* The level an object at nesting `level` (0: top) is captured in, or NULL */
static JfLevel *level_at(int level) {
    JfParser *p = &S->p;
    if (level < 0 || level >= p->nlv || !p->lv[level].ok) return NULL;
    return &p->lv[level];
}

/* An object opens at `level`: its values start over */
static void level_open(int level) {
    JfParser *p = &S->p;
    const JfConfig *c = S->cfg;
    if (c->depth_max && level >= c->depth_max) return;
    if (level >= p->nlv) {
        size_t cap = (size_t)p->nlv;
        size_t need = (size_t)level + 1;
        JfLevel *lv = p->lv;
        if (!jf_reserve((void **)&lv, &cap, need, sizeof(JfLevel))) {
            S->work->truncated = true;
            return;
        }
        memset(lv + p->nlv, 0, (cap - (size_t)p->nlv) * sizeof(JfLevel));
        p->lv = lv;
        p->nlv = (int)cap;
    }
    JfLevel *L = &p->lv[level];
    if (L->nkeys < c->nkeys) {
        size_t cap = (size_t)L->nkeys;
        if (!jf_reserve_words(&L->off, &cap, (size_t)c->nkeys)) {
            L->ok = false;
            S->work->truncated = true;
            return;
        }
        L->nkeys = (int)cap;
    }
    mini_words_fill(L->off, JF_NONE, (size_t)c->nkeys * sizeof(uint32_t));
    L->have = 0;
    L->used = 0;
    L->ok = true;
}

static bool has_key(const uint32_t *bits, int k) {
    return !bits || (bits[k / 32] >> (k % 32)) & 1u;
}

static uint32_t pool_put(JfSnap *sn, const char *s, size_t n) {
    const JfConfig *c = S->cfg;
    if (c->pool_max && sn->used + n + 1 > c->pool_max) {
        sn->truncated = true;
        return JF_NONE;
    }
    if (!jf_reserve((void **)&sn->pool, &sn->pcap, sn->used + n + 1, 1)) {
        sn->truncated = true;
        return JF_NONE;
    }
    memcpy(sn->pool + sn->used, s, n);
    sn->pool[sn->used + n] = '\0';
    uint32_t off = (uint32_t)sn->used;
    sn->used += n + 1;
    return off;
}

/* A row more in the snapshot; false when memory is short */
static bool snap_row(JfSnap *sn) {
    if (sn->count < sn->cap) return true;
    size_t cap = (size_t)sn->cap, cap2 = (size_t)sn->cap;
    size_t need = (size_t)sn->count + 1;
    if (!jf_reserve_words(&sn->sel, &cap, need)) return false;
    size_t width = (size_t)(sn->nkeys ? sn->nkeys : 1), offs = cap2 * width;
    if (!jf_reserve_words(&sn->off, &offs, cap * width)) return false;
    sn->cap = (int)cap;
    return true;
}

/* An object closed: list it, then keep it if a selector matches */
static void object_done(int level) {
    const JfConfig *c = S->cfg;
    JfLevel *L = level_at(level);
    if (!L || !L->have) return;
    JfSnap *sn = S->work;
    sn->objects++;
    if (S->dump) {
        for (int k = 0; k < c->nkeys; k++) {
            if (k) dump_append("\t", 1);
            if (L->off[k] != JF_NONE) dump_append(L->buf + L->off[k], strlen(L->buf + L->off[k]));
        }
        dump_append("\n", 1);
    }
    for (int s = 0; s < c->nsel; s++) {
        const JfSelector *sel = &c->sel[s];
        if (L->off[sel->key] == JF_NONE) continue;
        const char *v = L->buf + L->off[sel->key];
        bool hit = (sel->mode == JF_EQUAL) ? (strlen(v) == sel->len && memcmp(v, sel->text, sel->len) == 0)
                                           : (strncmp(v, sel->text, sel->len) == 0);
        if (!hit) continue;
        if (!snap_row(sn)) {
            sn->truncated = true;
            return;
        }
        int i = sn->count++;
        sn->sel[i] = (uint32_t)s;
        for (int k = 0; k < sn->nkeys; k++) {
            uint32_t o = L->off[k];
            sn->off[(size_t)i * (size_t)sn->nkeys + (size_t)k] =
                (o != JF_NONE && has_key(sel->keep, k)) ? pool_put(sn, L->buf + o, strlen(L->buf + o)) : JF_NONE;
        }
        return;
    }
}

/* A string or scalar token begins: store it when it is a key (no longer
 * than the longest configured one, plus one byte so a longer one never
 * matches) or the value of a captured key */
static void token_begin(void) {
    JfParser *p = &S->p;
    const JfConfig *c = S->cfg;
    p->tok_len = 0;
    if (!p->expect_value) {
        p->tok_keep = true;
        p->tok_limit = c->key_maxlen + 1;
    } else {
        p->tok_keep = p->key >= 0 && level_at(p->depth - 1) != NULL;
        p->tok_limit = c->value_max ? c->value_max : (size_t)-1;
    }
}

/* A complete string or scalar token */
static void token_done(void) {
    JfParser *p = &S->p;
    const JfConfig *c = S->cfg;
    if (!p->expect_value) {
        p->key = -1;
        if (p->tok_len <= c->key_maxlen) {
            for (int k = 0; k < c->nkeys; k++) {
                if (strlen(c->keys[k]) == p->tok_len && (p->tok_len == 0 || memcmp(p->tok, c->keys[k], p->tok_len) == 0)) {
                    p->key = k;
                    break;
                }
            }
        }
        return;
    }
    p->expect_value = false;
    JfLevel *L = level_at(p->depth - 1);
    if (p->key < 0 || !L || !p->tok_keep) return;
    size_t n = p->tok_len;
    if (n == p->tok_limit) n = utf8_trim_tail(p->tok, n);   /* cut at value-len (or by memory) */
    if (!jf_reserve((void **)&L->buf, &L->cap, L->used + n + 1, 1)) {
        S->work->truncated = true;
        return;
    }
    memcpy(L->buf + L->used, p->tok ? p->tok : "", n);
    L->buf[L->used + n] = '\0';
    if (L->off[p->key] == JF_NONE) L->have++;
    L->off[p->key] = (uint32_t)L->used;
    L->used += n + 1;
}

static void parse_bytes(const char *data, size_t len) {
    JfParser *p = &S->p;
    for (size_t i = 0; i < len; i++) {
        char c = data[i];
        if (p->in_str) {
            if (p->uhex > 0) {
                int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 :
                        (c >= 'A' && c <= 'F') ? c - 'A' + 10 : 0;
                p->ucode = (p->ucode << 4) | (unsigned)d;
                if (--p->uhex == 0) tok_put_escape(p, p->ucode);
            } else if (p->esc) {
                p->esc = false;
                if (c == 'u') {
                    p->uhex = 4;
                    p->ucode = 0;
                } else {
                    tok_flush_high(p);   /* a high surrogate left alone */
                    tok_put(p, c == 'n' ? '\n' : c == 't' ? '\t' : c == 'r' ? '\r' :
                               c == 'b' ? '\b' : c == 'f' ? '\f' : c);   /* \" \\ \/ as themselves */
                }
            } else if (c == '\\') {
                p->esc = true;
            } else if (c == '"') {
                tok_flush_high(p);
                p->in_str = false;
                token_done();
            } else {
                tok_flush_high(p);
                tok_put(p, c);
            }
            continue;
        }
        if (p->in_scalar) {
            if (c == ',' || c == '}' || c == ']' || c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                p->in_scalar = false;
                token_done();
            } else {
                tok_put(p, c);
                continue;
            }
        }
        switch (c) {
            case '"': p->in_str = true; token_begin(); break;
            case ':': p->expect_value = true; break;
            case ',': p->expect_value = false; break;
            case '[': p->expect_value = false; break;
            case '{':
                p->expect_value = false;
                level_open(p->depth);
                p->depth++;
                break;
            case '}':
                if (p->depth > 0) object_done(p->depth - 1);
                if (p->depth > 0) p->depth--;
                p->expect_value = false;
                break;
            case ']': case ' ': case '\t': case '\r': case '\n': break;
            default:
                if (p->expect_value) {   /* number, true, false, null */
                    p->in_scalar = true;
                    token_begin();
                    tok_put(p, c);
                }
                break;
        }
    }
}

/* ========================================================================= */
/* Feeding (shared by the ESP32 task and the WASM proxy)                      */
/* ========================================================================= */

static void snap_reset(JfSnap *sn, int nkeys) {
    if (sn->nkeys != nkeys) {   /* rows of another width: allocated again as they come */
        free(sn->sel);
        free(sn->off);
        sn->sel = NULL;
        sn->off = NULL;
        sn->cap = 0;
        sn->nkeys = nkeys;
    }
    sn->count = 0;
    sn->used = 0;
    sn->truncated = false;
    sn->objects = 0;
}

void mini_json_feed_begin(void) {
    if (!jf_alloc()) return;
    JF_LOCK();
    if (S->pending) {
        config_free(S->cfg);
        S->cfg = S->pending;
        S->pending = NULL;
    }
    S->ready = false; /* an untaken document is superseded */
    bool dump = s_want_dump;
    JF_UNLOCK();
    JfParser *p = &S->p;
    p->depth = 0;
    p->in_str = p->in_scalar = p->esc = p->expect_value = false;
    p->uhex = 0;
    p->ucode = p->uhigh = 0;
    p->key = -1;
    p->tok_keep = false;
    p->tok_len = 0;
    for (int l = 0; l < p->nlv; l++) p->lv[l].ok = false;
    S->bytes = 0;
    if (!S->cfg) return;
    snap_reset(S->work, S->cfg->nkeys);
    S->work->generation = S->cfg->generation;
    S->dump_len = 0;
    if (dump && !S->dump) {
        S->dump_cap = 0;
        jf_reserve((void **)&S->dump, &S->dump_cap, 1024, 1);
    }
    if (S->dump) S->dump[0] = '\0';
}

void mini_json_feed_data(const char *data, size_t len) {
    if (!S || !S->cfg || !data || !len) return;
    parse_bytes(data, len);
    S->bytes += (uint32_t)len;
}

void mini_json_feed_end(bool ok, const char *error) {
    if (!S) return;
    if (ok && (!S->cfg || S->work->objects == 0)) {
        ok = false;
        error = "no matching JSON objects in the response";
    }
    JF_LOCK();
    s_fetches++;
    s_last_bytes = S->bytes;
    if (ok) {
        S->ready = true;
        s_state = JF_OK;
        s_error[0] = '\0';
        s_last_ok_ms = jf_now_ms();
        if (S->dump) {
#if defined(ESP_PLATFORM)
            free(s_dump_pub); /* hand the listing over; the next request allocates anew */
            s_dump_pub = S->dump;
            s_dump_pub_len = S->dump_len;
            S->dump = NULL;
            S->dump_cap = 0;
            s_want_dump = false;
#else
            char *copy = jf_strdup(S->dump, S->dump_len);
            if (copy) {
                free(s_dump_pub);
                s_dump_pub = copy;
                s_dump_pub_len = S->dump_len;
            }
#endif
            s_dump_seq++;
        }
    } else {
        s_failures++;
        s_state = JF_ERROR;
        copy_str(s_error, sizeof(s_error), error ? error : "fetch failed");
    }
    JF_UNLOCK();
}

/* ========================================================================= */
/* fs-download: a file from the network into the app's folder                */
/* ========================================================================= */
/* One download at a time. The ESP32 runs it in the feed's task, before the
 * next document (the same plain HTTP fetch, so the device headers and the
 * https converter apply); the body goes to <path>.part, which replaces path
 * when it is complete. The WebREPL fetches it for the WASM device
 * (mini_download_take / mini_download_done). */

#define DL_MAX_BYTES  (256 * 1024)
#define DL_TIMEOUT_MS 15000
enum { DL_IDLE, DL_QUEUED, DL_RUNNING, DL_DONE, DL_ERROR };
static volatile int s_dl_state = DL_IDLE;
static char *s_dl_url = NULL;          /* the queued request, malloc'd */
static char s_dl_path[96];             /* the file in the app's folder (full path) */
static char s_dl_rel[96];              /* the path as the app gave it */
static uint32_t s_dl_max, s_dl_bytes, s_dl_seq;
static char s_dl_err[64];

static void dl_finish(bool ok, uint32_t bytes, const char *err) {
    JF_LOCK();
    s_dl_bytes = bytes;
    copy_str(s_dl_err, sizeof(s_dl_err), ok ? "" : (err && err[0] ? err : "download failed"));
    s_dl_state = ok ? DL_DONE : DL_ERROR;
    s_dl_seq++;
    free(s_dl_url);
    s_dl_url = NULL;
    JF_UNLOCK();
}

const char *mini_download_take(void) {
    if (s_dl_state != DL_QUEUED || !s_dl_url) return NULL;
    s_dl_state = DL_RUNNING;
    return s_dl_url;
}

const char *mini_download_path(void) { return s_dl_path; }
uint32_t mini_download_max(void) { return s_dl_max; }
void mini_download_done(bool ok, uint32_t bytes, const char *error) { dl_finish(ok, bytes, error); }

/* ========================================================================= */
/* ESP32 polling task                                                        */
/* ========================================================================= */

#if defined(ESP_PLATFORM)
static bool jf_wifi_up(void) {
    esp_netif_ip_info_t ip;
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    return sta && esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr != 0;
}

/* The queued fs-download, written through <path>.part */
static void jf_download(char *buf, int cap) {
    JF_LOCK();
    char *url = s_dl_url ? jf_strdup(s_dl_url, strlen(s_dl_url)) : NULL;
    char path[96], part[104];
    copy_str(path, sizeof(path), s_dl_path);
    uint32_t max = s_dl_max;
    s_dl_state = DL_RUNNING;
    JF_UNLOCK();
    snprintf(part, sizeof(part), "%s.part", path);
    char err[64] = "cannot connect";
    uint32_t total = 0;
    bool ok = false;
    if (!url) {
        copy_str(err, sizeof(err), "out of memory");
    } else if (!jf_wifi_up()) {
        copy_str(err, sizeof(err), "WiFi not connected");
    } else {
        int status = 0;
        int64_t length = -1;
        MiniFetch *h = mini_fetch_open(url, DL_TIMEOUT_MS, MINI_FETCH_REDIRECTS, &status, &length, err, sizeof(err));
        if (h) {
            if (status != 200) {
                snprintf(err, sizeof(err), "HTTP status %d", status);
            } else if (length > (int64_t)max) {
                snprintf(err, sizeof(err), "larger than %u KB", (unsigned)(max / 1024));
            } else {
                FILE *f = fopen(part, "wb");
                if (!f) {
                    copy_str(err, sizeof(err), "cannot create the file");
                } else {
                    bool bad = false;
                    int n;
                    while (!bad && (n = mini_fetch_read(h, buf, cap)) > 0) {
                        if (total + (uint32_t)n > max) {
                            snprintf(err, sizeof(err), "larger than %u KB", (unsigned)(max / 1024));
                            bad = true;
                        } else if (fwrite(buf, 1, (size_t)n, f) != (size_t)n) {
                            copy_str(err, sizeof(err), "write failed (storage full?)");
                            bad = true;
                        } else {
                            total += (uint32_t)n;
                        }
                    }
                    if (!bad && n < 0) {
                        snprintf(err, sizeof(err), "read error after %u bytes", (unsigned)total);
                        bad = true;
                    }
                    if (fclose(f) != 0 && !bad) {
                        copy_str(err, sizeof(err), "write failed (storage full?)");
                        bad = true;
                    }
                    if (!bad) {
                        remove(path);
                        ok = rename(part, path) == 0;
                        if (!ok) copy_str(err, sizeof(err), "rename failed");
                    }
                    if (!ok) remove(part);
                }
            }
            mini_fetch_close(h);
        }
    }
    free(url);
    dl_finish(ok, ok ? total : 0, err);
}

static void jf_task(void *arg) {
    (void)arg;
    char buf[512];
    for (;;) {
        if (s_dl_state == DL_QUEUED) {     /* a download goes before the next document */
            jf_download(buf, (int)sizeof(buf));
            continue;
        }
        char *url = NULL;
        uint32_t interval = 1000;
        JF_LOCK();
        const JfConfig *c = S ? (S->pending ? S->pending : S->cfg) : NULL;
        if (c && c->url[0]) {
            url = jf_strdup(c->url, strlen(c->url));
            interval = c->interval_ms;
        }
        JF_UNLOCK();
        if (!url) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
            continue;
        }
        int64_t t0 = jf_now_ms();
        esp_netif_ip_info_t ip;
        esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (!sta || esp_netif_get_ip_info(sta, &ip) != ESP_OK || ip.ip.addr == 0) {
            free(url);
            mini_json_feed_begin();
            mini_json_feed_end(false, "WiFi not connected");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (s_state != JF_OK) s_state = JF_CONNECTING;
        /* a fast poll (the PC monitor, 1 s) gives up soon; a slow feed waits
         * for a proxy that fetches its sources first (up to 15 s) */
        int timeout = interval < 3000 ? 3000 : (interval > 15000 ? 15000 : (int)interval);
        /* plain HTTP, the device id and password in headers (minischeme_channel.h) */
        bool ok = false;
        char err[64] = "cannot connect";
        int status = 0;
        int64_t length = -1;
        mini_json_feed_begin();
        MiniFetch *h = mini_fetch_open(url, timeout, 0, &status, &length, err, sizeof(err));
        if (!h && status == MINI_FETCH_WAIT) {
            /* not ready yet (the network time): still connecting, not a failure;
             * json-feed-status says what it waits for */
            free(url);
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
            continue;
        }
        if (h) {
            if (status == 200) {
                int n;
                uint32_t total = 0;
                while ((n = mini_fetch_read(h, buf, sizeof(buf))) > 0) {
                    mini_json_feed_data(buf, (size_t)n);
                    total += (uint32_t)n;
                }
                ok = (n == 0);
                if (!ok) snprintf(err, sizeof(err), "read error after %u bytes", (unsigned)total);
            } else {
                snprintf(err, sizeof(err), "HTTP status %d", status);
            }
            mini_fetch_close(h);
        }
        free(url);
        s_last_ms = (uint32_t)(jf_now_ms() - t0);
        mini_json_feed_end(ok, err);
        /* a failed fetch is retried within JF_RETRY_MS, not after a long interval */
        int64_t period = ok || interval < JF_RETRY_MS ? (int64_t)interval : JF_RETRY_MS;
        int64_t wait = period - (jf_now_ms() - t0);
        /* json-feed-start notifies the task: a new URL (an app page switch)
         * is fetched at once instead of after the old interval */
        vTaskDelay(pdMS_TO_TICKS(50));
        if (wait > 50) ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait - 50));
    }
}
#endif

/* ========================================================================= */
/* Scheme primitives                                                         */
/* ========================================================================= */

void mini_json_feed_set_https_proxy(const char *base) {
    size_t n = base ? strlen(base) : 0;
    while (n > 0 && base[n - 1] == '/') n--;
    free(s_https_proxy);
    s_https_proxy = n ? jf_strdup(base, n) : NULL;
}

/* url as the board fetches it (malloc'd): an https:// one through the
 * converter's /fetch; NULL (with why) when there is no converter. On the
 * ESP32 mini_fetch_open sends it to the converter itself as
 * <it>/fetch?url=.... */
static char *feed_url(const char *url, const char **why) {
#if defined(ESP_PLATFORM)
    bool direct = s_https_proxy != NULL;
#else
    bool direct = false;
#endif
    if (direct || strncmp(url, "https://", 8) != 0) {
        char *u = jf_strdup(url, strlen(url));
        if (!u) *why = "out of memory";
        return u;
    }
    if (!s_https_proxy) {
        *why = "an https:// URL needs a protocol converter: config.ini [app] https_proxy or proxy";
        return NULL;
    }
    static const char hex[] = "0123456789ABCDEF";
    size_t cap = strlen(s_https_proxy) + 12 + strlen(url) * 3 + 1;
    char *out = (char *)malloc(cap);
    if (!out) {
        *why = "out of memory";
        return NULL;
    }
    size_t n = (size_t)snprintf(out, cap, "%s/fetch?url=", s_https_proxy);
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

static int list_length(Value l) {
    int n = 0;
    for (; mini_is_pair(l); l = mini_cdr(l)) n++;
    return n;
}

/* An optional size argument: a positive integer, else 0 (no limit) */
static size_t opt_size(int argc, Value *argv, int i) {
    if (argc <= i || !mini_is_fixnum(argv[i])) return 0;
    int64_t v = mini_to_fixnum(argv[i]);
    return v > 0 ? (size_t)v : 0;
}

/* (json-feed-start url interval-ms keys selectors [pool-bytes [value-len [depth]]])
 * pool-bytes: value text kept per document; value-len: bytes kept per value;
 * depth: objects captured up to this nesting level. Each one omitted, 0 or
 * #f: no limit (only memory bounds it). */
static Value prim_json_feed_start(MiniVM *vm, int argc, Value *argv) {
    if (argc < 4 || !mini_is_string(argv[0]) || !mini_is_fixnum(argv[1])) {
        vm_error(vm, "json-feed-start: expected url interval-ms keys selectors [pool-bytes [value-len [depth]]]");
        return mini_void();
    }
    JfConfig *c = (JfConfig *)calloc(1, sizeof(JfConfig));
    if (!c || !jf_alloc()) {
        free(c);
        vm_error(vm, "json-feed-start: out of memory");
        return mini_void();
    }
    const char *why = NULL;
    c->url = feed_url(mini_string_data(argv[0]), &why);
    if (!c->url) {
        config_free(c);
        vm_error(vm, "json-feed-start: %s", why);
        return mini_void();
    }
    int64_t iv = mini_to_fixnum(argv[1]);
    c->interval_ms = (uint32_t)(iv < 200 ? 200 : iv);
    int nkeys = list_length(argv[2]), nsel = list_length(argv[3]);
    c->keys = (char **)calloc((size_t)(nkeys ? nkeys : 1), sizeof(char *));
    c->sel = (JfSelector *)calloc((size_t)(nsel ? nsel : 1), sizeof(JfSelector));
    if (!c->keys || !c->sel) {
        config_free(c);
        vm_error(vm, "json-feed-start: out of memory");
        return mini_void();
    }
    for (Value l = argv[2]; mini_is_pair(l); l = mini_cdr(l)) {
        Value k = mini_car(l);
        if (!mini_is_string(k)) {
            config_free(c);
            vm_error(vm, "json-feed-start: keys must be strings");
            return mini_void();
        }
        size_t kl = mini_string_len(k);
        c->keys[c->nkeys] = jf_strdup(mini_string_data(k), kl);
        if (!c->keys[c->nkeys]) {
            config_free(c);
            vm_error(vm, "json-feed-start: out of memory");
            return mini_void();
        }
        c->nkeys++;
        if (kl > c->key_maxlen) c->key_maxlen = kl;
    }
    int kwords = (c->nkeys + 31) / 32;
    for (Value l = argv[3]; mini_is_pair(l); l = mini_cdr(l)) {
        Value s = mini_car(l);
        Value k = mini_is_pair(s) ? mini_car(s) : mini_void();
        Value m = mini_is_pair(s) && mini_is_pair(mini_cdr(s)) ? mini_car(mini_cdr(s)) : mini_void();
        Value t = mini_is_pair(s) && mini_is_pair(mini_cdr(s)) && mini_is_pair(mini_cdr(mini_cdr(s)))
                  ? mini_car(mini_cdr(mini_cdr(s))) : mini_void();
        const char *ms = mini_is_symbol(m) ? mini_symbol_name(m) : "";
        if (!mini_is_fixnum(k) || mini_to_fixnum(k) < 0 || mini_to_fixnum(k) >= c->nkeys ||
            (strcmp(ms, "equal") && strcmp(ms, "prefix")) || !mini_is_string(t)) {
            config_free(c);
            vm_error(vm, "json-feed-start: selector must be (key-index 'equal|'prefix string)");
            return mini_void();
        }
        JfSelector *sel = &c->sel[c->nsel];
        sel->key = (int)mini_to_fixnum(k);
        sel->mode = strcmp(ms, "equal") == 0 ? JF_EQUAL : JF_PREFIX;
        sel->len = mini_string_len(t);
        sel->text = jf_strdup(mini_string_data(t), sel->len);
        c->nsel++;   /* counted now: config_free frees what it holds */
        if (!sel->text) {
            config_free(c);
            vm_error(vm, "json-feed-start: out of memory");
            return mini_void();
        }
        for (Value r = mini_cdr(mini_cdr(mini_cdr(s))); mini_is_pair(r); r = mini_cdr(r)) {
            Value kk = mini_car(r);
            if (!mini_is_fixnum(kk) || mini_to_fixnum(kk) < 0 || mini_to_fixnum(kk) >= c->nkeys) continue;
            if (!sel->keep) {
                sel->keep = (uint32_t *)calloc((size_t)kwords, sizeof(uint32_t));
                if (!sel->keep) {
                    config_free(c);
                    vm_error(vm, "json-feed-start: out of memory");
                    return mini_void();
                }
            }
            int64_t kv = mini_to_fixnum(kk);
            sel->keep[kv / 32] |= 1u << (kv % 32);
        }
    }
    c->pool_max = opt_size(argc, argv, 4);
    c->value_max = opt_size(argc, argv, 5);
    c->depth_max = (int)opt_size(argc, argv, 6);
#if defined(ESP_PLATFORM)
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
#endif
    JF_LOCK();
    c->generation = ++s_latest_gen;
    config_free(S->pending);
    S->pending = c;
    if (s_state == JF_IDLE) s_state = JF_CONNECTING;
    JF_UNLOCK();
#if defined(ESP_PLATFORM)
    if (!s_task) xTaskCreate(jf_task, "jsonfeed", 4096, NULL, 4, &s_task);   /* about 2.7 KB used */
    else xTaskNotifyGive(s_task);
#endif
    return mini_true();
}

/* (json-feed-stop): forget the URL; the fetcher idles */
static Value prim_json_feed_stop(MiniVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc; (void)argv;
    if (!S) return mini_void();
    JF_LOCK();
    if (S->pending) S->pending->url[0] = '\0';
    else if (S->cfg) S->cfg->url[0] = '\0';
    s_state = JF_IDLE;
    JF_UNLOCK();
    return mini_void();
}

static bool view_current(void) {
    return S && s_seq > 0 && S->view->generation == s_latest_gen;
}

/* (json-feed-seq): takes the newest complete document, if any */
static Value prim_json_feed_seq(MiniVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc; (void)argv;
    if (!S) return mini_make_fixnum(0);
    JF_LOCK();
    if (S->ready) {
        JfSnap *t = S->view;
        S->view = S->work;
        S->work = t;
        S->ready = false;
        s_seq++;
    }
    JF_UNLOCK();
    return mini_make_fixnum(view_current() ? (int64_t)s_seq : 0);
}

static Value prim_json_feed_count(MiniVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc; (void)argv;
    return mini_make_fixnum(view_current() ? S->view->count : 0);
}

static bool obj_index(int argc, Value *argv, int *i) {
    if (argc < 1 || !mini_is_fixnum(argv[0]) || !view_current()) return false;
    int64_t n = mini_to_fixnum(argv[0]);
    if (n < 0 || n >= S->view->count) return false;
    *i = (int)n;
    return true;
}

static Value prim_json_feed_selector(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    int i;
    return obj_index(argc, argv, &i) ? mini_make_fixnum(S->view->sel[i]) : mini_false();
}

/* Key k of a field call: an index, or a key's name (of the taken document's
 * configuration); -1 when there is none */
static int field_key(Value k) {
    if (mini_is_fixnum(k)) {
        int64_t n = mini_to_fixnum(k);
        return n >= 0 && n < S->view->nkeys ? (int)n : -1;
    }
    if (!mini_is_string(k)) return -1;
    int found = -1;
    JF_LOCK();   /* the fetcher replaces the configuration */
    const JfConfig *c = S->cfg && S->cfg->generation == S->view->generation ? S->cfg : NULL;
    if (c) {
        for (int j = 0; j < c->nkeys && j < S->view->nkeys; j++) {
            if (strcmp(c->keys[j], mini_string_data(k)) == 0) { found = j; break; }
        }
    }
    JF_UNLOCK();
    return found;
}

/* The pool text of row i, key argv[1]; NULL when absent */
static const char *field_text(int argc, Value *argv) {
    int i;
    if (argc < 2 || !obj_index(argc, argv, &i)) return NULL;
    int k = field_key(argv[1]);
    if (k < 0) return NULL;
    uint32_t o = S->view->off[(size_t)i * (size_t)S->view->nkeys + (size_t)k];
    return o == JF_NONE ? NULL : S->view->pool + o;
}

static Value prim_json_feed_field(MiniVM *vm, int argc, Value *argv) {
    const char *v = field_text(argc, argv);
    return v ? mini_make_string(vm, v) : mini_false();
}

/* (json-feed-ints i k) => the integers of a comma-separated value
 * ("12,-3,7" -> (12 -3 7)), or #f: series (temperatures, prices) in one field */
static Value prim_json_feed_ints(MiniVM *vm, int argc, Value *argv) {
    const char *v = field_text(argc, argv);
    if (!v) return mini_false();
    size_t cap = 1;
    for (const char *q = v; *q; q++) cap += *q == ',';
    int64_t *nums = (int64_t *)malloc(cap * sizeof(int64_t));
    if (!nums) {
        vm_error(vm, "json-feed-ints: out of memory");
        return mini_void();
    }
    size_t n = 0;
    while (*v && n < cap) {
        while (*v == ',' || *v == ' ') v++;
        if (!*v) break;
        bool neg = *v == '-';
        if (neg || *v == '+') v++;
        int64_t x = 0;
        bool digits = false;
        while (*v >= '0' && *v <= '9') { x = x * 10 + (*v++ - '0'); digits = true; }
        while (*v && *v != ',') v++; /* skip anything else up to the next comma */
        if (digits) nums[n++] = neg ? -x : x;
    }
    Value list = mini_nil();
    mini_push_root(vm, &list);
    for (size_t j = n; j > 0; j--) list = mini_make_cons(vm, mini_make_fixnum(nums[j - 1]), list);
    mini_pop_root(vm);
    free(nums);
    return list;
}

static Value prim_json_feed_url(MiniVM *vm, int argc, Value *argv) {
    (void)argc; (void)argv;
    JF_LOCK(); /* the fetcher frees a replaced config */
    const char *u = mini_json_feed_url();
    char *url = u[0] ? jf_strdup(u, strlen(u)) : NULL;
    JF_UNLOCK();
    Value r = url ? mini_make_string(vm, url) : mini_false();
    free(url);
    return r;
}

static void alist_push(MiniVM *vm, Value *lst, const char *key, Value v) {
    mini_push_root(vm, &v);
    Value k = mini_make_symbol(vm, key);
    mini_push_root(vm, &k);
    Value cell = mini_make_cons(vm, k, v);
    mini_pop_root(vm);
    mini_pop_root(vm);
    mini_push_root(vm, &cell);
    *lst = mini_make_cons(vm, cell, *lst);
    mini_pop_root(vm);
}

static Value prim_json_feed_status(MiniVM *vm, int argc, Value *argv) {
    (void)argc; (void)argv;
    static const char *const states[] = { "idle", "connecting", "ok", "error" };
    char err[64];
    JF_LOCK();
    int state = s_state;
    copy_str(err, sizeof(err), s_error);
    uint32_t fetches = s_fetches, failures = s_failures, bytes = s_last_bytes, ms = s_last_ms;
    int64_t age = s_last_ok_ms ? jf_now_ms() - s_last_ok_ms : -1;
    size_t memory = jf_memory();
    JF_UNLOCK();
    int objects = view_current() ? S->view->objects : 0;
    bool truncated = view_current() && S->view->truncated;
    Value lst = mini_nil();
    mini_push_root(vm, &lst);
    alist_push(vm, &lst, "memory", mini_make_fixnum((int64_t)memory));
    alist_push(vm, &lst, "truncated", mini_make_bool(truncated));
    alist_push(vm, &lst, "objects", mini_make_fixnum(objects));
    alist_push(vm, &lst, "age-ms", mini_make_fixnum(age));
    alist_push(vm, &lst, "fetch-ms", mini_make_fixnum(ms));
    alist_push(vm, &lst, "bytes", mini_make_fixnum(bytes));
    alist_push(vm, &lst, "failures", mini_make_fixnum(failures));
    alist_push(vm, &lst, "fetches", mini_make_fixnum(fetches));
    alist_push(vm, &lst, "error", mini_make_string(vm, err));
#if defined(ESP_PLATFORM)
    {   /* what the fetch waits for now: the network time, a registration (and its server) */
        static const char *const steps[] = { "none", "clock", "register", "wifi" };
        char server[96];
        int step = mini_chan_step(server, sizeof(server));
        alist_push(vm, &lst, "step-server", mini_make_string(vm, server));
        alist_push(vm, &lst, "step", mini_make_symbol(vm, steps[step >= 0 && step <= 3 ? step : 0]));
    }
#endif
    alist_push(vm, &lst, "state", mini_make_symbol(vm, states[state]));
    mini_pop_root(vm);
    return lst;
}

/* (json-feed-dump) => listing of the next (ESP32) or last document, or #f */
static Value prim_json_feed_dump(MiniVM *vm, int argc, Value *argv) {
    (void)argc; (void)argv;
#if defined(ESP_PLATFORM)
    if (!s_task) return mini_false();
    uint32_t seen = s_dump_seq;
    s_want_dump = true;
    for (int i = 0; i < 120 && s_dump_seq == seen; i++) vTaskDelay(pdMS_TO_TICKS(50));
    if (s_dump_seq == seen) return mini_false();
#endif
    JF_LOCK();
    Value r = s_dump_pub ? mini_make_string_len(vm, s_dump_pub, s_dump_pub_len) : mini_false();
#if defined(ESP_PLATFORM)
    free(s_dump_pub); /* keep it only while someone asks */
    s_dump_pub = NULL;
    s_dump_pub_len = 0;
#endif
    JF_UNLOCK();
    return r;
}

/* (fs-download url path) => #t: queued; #f: another download is running */
static Value prim_fs_download(MiniVM *vm, int argc, Value *argv) {
    if (argc < 2 || !mini_is_string(argv[0]) || !mini_is_string(argv[1])) {
        vm_error(vm, "fs-download: expected (fs-download url path)");
        return mini_void();
    }
    const char *url = mini_string_data(argv[0]);
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        vm_error(vm, "fs-download: the URL must start with http:// or https://");
        return mini_void();
    }
    char full[96];
    if (!mini_access_file(vm, "fs-download", mini_string_data(argv[1]), true, full, sizeof(full))) return mini_void();
    if (s_dl_state == DL_QUEUED || s_dl_state == DL_RUNNING) return mini_false();
    size_t room = mini_app_storage_room();
    if (room < 1024) {
        vm_error(vm, "fs-download: the app's storage is full");
        return mini_void();
    }
    char *copy = jf_strdup(url, strlen(url));
    if (!copy) {
        vm_error(vm, "fs-download: out of memory");
        return mini_void();
    }
#if defined(ESP_PLATFORM)
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
#endif
    JF_LOCK();
    free(s_dl_url);
    s_dl_url = copy;
    copy_str(s_dl_path, sizeof(s_dl_path), full);
    copy_str(s_dl_rel, sizeof(s_dl_rel), mini_string_data(argv[1]));
    s_dl_max = room < DL_MAX_BYTES ? (uint32_t)room : DL_MAX_BYTES;
    s_dl_bytes = 0;
    s_dl_err[0] = '\0';
    s_dl_state = DL_QUEUED;
    JF_UNLOCK();
#if defined(ESP_PLATFORM)
    if (!s_task) xTaskCreate(jf_task, "jsonfeed", 4096, NULL, 4, &s_task);
    else xTaskNotifyGive(s_task);
#endif
    return mini_true();
}

/* (fs-download-status) => ((state . s) (path . p) (bytes . n) (error . e) (seq . k)) */
static Value prim_fs_download_status(MiniVM *vm, int argc, Value *argv) {
    (void)argc; (void)argv;
    static const char *const states[] = { "idle", "queued", "running", "done", "error" };
    char err[64], rel[96];
    JF_LOCK();
    int state = s_dl_state;
    uint32_t bytes = s_dl_bytes, seq = s_dl_seq;
    copy_str(err, sizeof(err), s_dl_err);
    copy_str(rel, sizeof(rel), s_dl_rel);
    JF_UNLOCK();
    Value lst = mini_nil();
    mini_push_root(vm, &lst);
    alist_push(vm, &lst, "seq", mini_make_fixnum((int64_t)seq));
    alist_push(vm, &lst, "error", mini_make_string(vm, err));
    alist_push(vm, &lst, "bytes", mini_make_fixnum((int64_t)bytes));
    alist_push(vm, &lst, "path", mini_make_string(vm, rel));
    alist_push(vm, &lst, "state", mini_make_symbol(vm, states[state]));
    mini_pop_root(vm);
    return lst;
}

void mini_register_json_feed_lib(MiniVM *vm) {
    static const MiniPrimEntry funcs[] = {
        { "fs-download",        prim_fs_download },
        { "fs-download-status", prim_fs_download_status },
        { "json-feed-start",    prim_json_feed_start },
        { "json-feed-stop",     prim_json_feed_stop },
        { "json-feed-seq",      prim_json_feed_seq },
        { "json-feed-count",    prim_json_feed_count },
        { "json-feed-selector", prim_json_feed_selector },
        { "json-feed-field",    prim_json_feed_field },
        { "json-feed-ints",     prim_json_feed_ints },
        { "json-feed-url",      prim_json_feed_url },
        { "json-feed-status",   prim_json_feed_status },
        { "json-feed-dump",     prim_json_feed_dump },
    };
    mini_register_prim_table(vm, funcs, sizeof(funcs) / sizeof(funcs[0]));
}

/* The feed's heap blocks, for the heap map */
void mini_json_feed_mem_owners(MiniMemOwnerFn fn, void *user) {
    if (!S) return;
    fn(S, "feed state", user);
    const JfConfig *cs[2] = { S->cfg, S->pending };
    for (int i = 0; i < 2; i++) {
        if (!cs[i]) continue;
        fn(cs[i], "feed config", user);
        fn(cs[i]->url, "feed url", user);
        fn(cs[i]->keys, "feed keys", user);
        fn(cs[i]->sel, "feed selectors", user);
    }
    const JfSnap *sn[2] = { &S->a, &S->b };
    for (int i = 0; i < 2; i++) {
        fn(sn[i]->pool, "feed text pool", user);
        fn(sn[i]->sel, "feed row selectors", user);
        fn(sn[i]->off, "feed row offsets", user);
    }
    fn(S->p.lv, "feed levels", user);
    for (int l = 0; l < S->p.nlv; l++) {
        fn(S->p.lv[l].buf, "feed level text", user);
        fn(S->p.lv[l].off, "feed level offsets", user);
    }
    fn(S->p.tok, "feed token", user);
    fn(S->dump, "feed dump", user);
}
