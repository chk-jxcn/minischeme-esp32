/**
 * @file minischeme_fontnet.c
 * @brief Glyphs from the font server: a file of hashed slots, a small RAM
 *        cache, and one request at a time. See minischeme_fontnet.h.
 *
 * The glyph file /spiffs/fonts/net-<face>-<height>.mgh, little-endian:
 *   0  "MGH1", u8 height, u8 max bytes per row, u16 reserved,
 *   8  u32 hash (the server's identity of the glyphs), u32 slot count,
 *  16  char face[16]
 *  32  slots of 6 + max_bpr * height bytes: u32 code point (0: empty),
 *      u8 advance, u8 bytes per row (0: the server has no such glyph), rows
 * A code point lives in the first free slot of the 8 from its hash; when
 * all are taken its first slot is overwritten. A new hash (the server's fonts
 * changed) or another face or height starts the file again.
 *
 * Server reply (font_server.py, fontsrv/glyphs.py), "MGLY" version 1:
 *   0  "MGLY", u8 version, u8 height, u8 bpp, u8 flags, u32 hash, u16 count,
 *      u16 reserved; then count records: u32 cp, u8 advance, u8 bpr, rows.
 */

#include "minischeme_fontnet.h"
#include "minischeme_gui.h"
#include "minischeme_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(__wasm__)
#include <sys/stat.h>
#endif

#if defined(ESP_PLATFORM)
#include "esp_timer.h"
static int64_t fn_now_ms(void) { return esp_timer_get_time() / 1000; }
#elif defined(__wasm__)
extern double js_now_ms(void);
static int64_t fn_now_ms(void) { return (int64_t)js_now_ms(); }
#else
#include <time.h>
static int64_t fn_now_ms(void) { return (int64_t)time(NULL) * 1000; }
#endif

#define FN_HEADER      32
#define FN_PROBES      8
#define FN_SLOTS_GAPS  512     /* board fonts first: only what they lack */
#define FN_SLOTS_FIRST 2048    /* server first: the face's common characters */
#define FN_CACHE_GAPS  16      /* RAM cache entries (two-way sets) */
#define FN_CACHE_FIRST 64
#define FN_WANT        64      /* code points waiting */
#define FN_PER_REQUEST 48      /* per request: the board's HTTP request line holds ~500 bytes */
#define FN_URL_MAX     512
#define FN_REPLY_MAX   (16 * 1024)
#define FN_FACE_MAX    16
#define FN_REC_MAX     (6 + 8 * 32)    /* a slot of a system font up to 32 px tall */

/* configuration */
static char s_url[128] = MINI_FONTNET_DEFAULT_URL;
static bool s_on = true, s_first = false;
static char s_face_cfg[FN_FACE_MAX] = "";

/* the open glyph file */
static char s_face[FN_FACE_MAX];
static int s_height = 0, s_max_bpr = 0, s_rec = 0;
static uint32_t s_slots = 0, s_hash = 0;
static bool s_open = false;
static char s_path[64];

/* RAM cache: two-way sets of [cp u32][adv u8][bpr u8][age u8][pad][rows] */
static uint8_t *s_cache = NULL;
static int s_cache_n = 0, s_entry = 0;
static uint8_t s_age = 0;

/* requests */
static uint32_t s_want[FN_WANT], s_fly[FN_WANT];
static int s_nwant = 0, s_nfly = 0;
static int64_t s_retry_at = 0;
static int s_failures = 0;
static uint32_t s_requests = 0, s_received = 0, s_absent = 0;
static char s_error[64] = "";
static char s_req_url[FN_URL_MAX];

static uint8_t s_rec_buf[FN_REC_MAX]; /* one slot (main thread only) */
static MiniFont s_font;               /* what the glyphs are drawn as: the system font's height */
static MiniGlyph s_views[4];
static int s_view = 0;

/* ========================================================================= */
/* Storage                                                                   */
/* ========================================================================= */

static FILE *s_fp = NULL;             /* the stdio storage keeps the file open */
static char s_fp_path[64];

static void io_close(void) {
    if (s_fp) fclose(s_fp);
    s_fp = NULL;
}

static FILE *io_open(const char *path, bool create) {
    if (s_fp && strcmp(s_fp_path, path) == 0) return s_fp;
    io_close();
    s_fp = fopen(path, "r+b");
#if !defined(__wasm__)
    if (!s_fp && create) mkdir("/spiffs/fonts", 0777);   /* a board without font files yet */
#endif
    if (!s_fp && create) s_fp = fopen(path, "w+b");
    if (s_fp) {
        /* no stdio buffer: newlib would take the file system's block size
         * (4 KB on LittleFS) for one, and LittleFS caches reads itself */
        setvbuf(s_fp, NULL, _IONBF, 0);
        snprintf(s_fp_path, sizeof(s_fp_path), "%s", path);
    }
    return s_fp;
}

static long std_size(const char *path) {
    FILE *f = io_open(path, false);
    if (!f || fseek(f, 0, SEEK_END) != 0) return -1;
    return ftell(f);
}

static bool std_read(const char *path, long off, void *buf, size_t n) {
    FILE *f = io_open(path, false);
    return f && fseek(f, off, SEEK_SET) == 0 && fread(buf, 1, n, f) == n;
}

static bool std_write(const char *path, long off, const void *buf, size_t n) {
    FILE *f = io_open(path, true);
    if (!f || fseek(f, 0, SEEK_END) != 0) return false;
    long end = ftell(f);
    static const uint8_t zeros[64] = { 0 };
    while (end >= 0 && end < off) {   /* a slot past the end: the gap is empty slots */
        size_t k = (size_t)(off - end) < sizeof(zeros) ? (size_t)(off - end) : sizeof(zeros);
        if (fwrite(zeros, 1, k, f) != k) return false;
        end += (long)k;
    }
    bool ok = fseek(f, off, SEEK_SET) == 0 && fwrite(buf, 1, n, f) == n;
    return fflush(f) == 0 && ok;
}

static void std_remove(const char *path) {
    if (s_fp && strcmp(s_fp_path, path) == 0) io_close();
    remove(path);
}

static const MiniFontnetIO s_std_io = { std_size, std_read, std_write, std_remove };
static const MiniFontnetIO *s_io = &s_std_io;

void mini_fontnet_set_io(const MiniFontnetIO *io) {
    io_close();
    s_io = io ? io : &s_std_io;
    s_open = false;
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ========================================================================= */
/* The glyph file and the RAM cache                                          */
/* ========================================================================= */

static void cache_clear(void) {
    if (s_cache) memset(s_cache, 0, (size_t)s_cache_n * (size_t)s_entry);
}

static void store_close(void) {
    io_close();
    free(s_cache);
    s_cache = NULL;
    s_open = false;
    s_nwant = s_nfly = 0;
}

static void write_header(void) {
    uint8_t h[FN_HEADER] = { 'M', 'G', 'H', '1' };
    h[4] = (uint8_t)s_height;
    h[5] = (uint8_t)s_max_bpr;
    put32(h + 8, s_hash);
    put32(h + 12, s_slots);
    memcpy(h + 16, s_face, strlen(s_face) < FN_FACE_MAX ? strlen(s_face) : FN_FACE_MAX);
    s_io->write(s_path, 0, h, sizeof(h));
}

/* The file for the face and height asked for now; false when off */
static bool store_ready(void) {
    if (!s_on || !s_url[0]) return false;
    int height = 16;
    const char *face = mini_font_system_face(&height);
    if (s_face_cfg[0]) face = s_face_cfg;
    if (s_open && s_height == height && strcmp(s_face, face) == 0) return true;
    store_close();
    snprintf(s_face, sizeof(s_face), "%s", face);
    s_height = height;
    s_max_bpr = (2 * height + 7) / 8;          /* up to two cells wide (emoji) */
    s_rec = 6 + s_max_bpr * height;
    s_slots = s_first ? FN_SLOTS_FIRST : FN_SLOTS_GAPS;
    s_cache_n = s_first ? FN_CACHE_FIRST : FN_CACHE_GAPS;
    s_entry = (8 + s_max_bpr * height + 3) & ~3;
    s_cache = (uint8_t *)calloc((size_t)s_cache_n, (size_t)s_entry);
    if (!s_cache) return false;
    snprintf(s_path, sizeof(s_path), "/spiffs/fonts/net-%s-%d.mgh", s_face, s_height);
    uint8_t h[FN_HEADER];
    s_hash = 0;
    if (s_io->size(s_path) >= FN_HEADER && s_io->read(s_path, 0, h, sizeof(h)) && memcmp(h, "MGH1", 4) == 0 &&
        h[4] == s_height && h[5] == s_max_bpr && get32(h + 12) == s_slots) {
        s_hash = get32(h + 8);
    } else {
        s_io->remove(s_path);   /* another layout: start again */
    }
    memset(&s_font, 0, sizeof(s_font));
    s_font.id = -1;
    s_font.name = "fontnet";
    s_font.height = (uint8_t)s_height;
    const MiniFont *sys = mini_font_get(MINI_FONT_SYSTEM_16);
    s_font.ascent = sys && sys->height == s_height ? sys->ascent : 0;
    s_open = true;
    return true;
}

static uint32_t slot_of(uint32_t cp) {
    return (uint32_t)((cp * 2654435761u) % s_slots);
}

static long slot_off(uint32_t slot) {
    return (long)FN_HEADER + (long)slot * s_rec;
}

/* The cache entry of cp, NULL when not cached */
static uint8_t *cache_find(uint32_t cp) {
    int set = (int)((cp * 2246822519u) % (uint32_t)(s_cache_n / 2)) * 2;
    for (int w = 0; w < 2; w++) {
        uint8_t *e = s_cache + (size_t)(set + w) * s_entry;
        if (get32(e) == cp) {
            e[6] = ++s_age;
            return e;
        }
    }
    return NULL;
}

/* A cache entry for cp (the older of its set) */
static uint8_t *cache_put(uint32_t cp, uint8_t adv, uint8_t bpr, const uint8_t *rows) {
    int set = (int)((cp * 2246822519u) % (uint32_t)(s_cache_n / 2)) * 2;
    uint8_t *a = s_cache + (size_t)set * s_entry, *b = a + s_entry;
    uint8_t *e = get32(a) == 0 ? a : get32(b) == 0 ? b : (uint8_t)(s_age - a[6]) >= (uint8_t)(s_age - b[6]) ? a : b;
    put32(e, cp);
    e[4] = adv;
    e[5] = bpr;
    e[6] = ++s_age;
    if (bpr && rows) memcpy(e + 8, rows, (size_t)bpr * s_height);
    return e;
}

/* The slot holding cp in the file: its bytes into a cache entry; NULL when
 * the file does not know cp */
static uint8_t *file_find(uint32_t cp) {
    uint8_t *rec = s_rec_buf;
    if (s_rec > FN_REC_MAX || s_hash == 0) return NULL;
    uint32_t home = slot_of(cp);
    for (int i = 0; i < FN_PROBES; i++) {
        uint32_t slot = (home + (uint32_t)i) % s_slots;
        if (!s_io->read(s_path, slot_off(slot), rec, 6)) return NULL;   /* past the end: empty */
        uint32_t c = get32(rec);
        if (c == 0) return NULL;
        if (c != cp) continue;
        uint8_t bpr = rec[5];
        if (bpr > s_max_bpr) bpr = 0;
        if (bpr && !s_io->read(s_path, slot_off(slot) + 6, rec + 6, (size_t)bpr * s_height)) return NULL;
        return cache_put(cp, rec[4], bpr, rec + 6);
    }
    return NULL;
}

static void file_put(uint32_t cp, uint8_t adv, uint8_t bpr, const uint8_t *rows) {
    uint8_t *rec = s_rec_buf;
    if (s_rec > FN_REC_MAX) return;
    uint32_t home = slot_of(cp), slot = home;
    for (int i = 0; i < FN_PROBES; i++) {
        uint32_t k = (home + (uint32_t)i) % s_slots;
        uint8_t h[4];
        if (!s_io->read(s_path, slot_off(k), h, 4) || get32(h) == 0 || get32(h) == cp) {
            slot = k;
            break;
        }
    }
    memset(rec, 0, (size_t)s_rec);
    put32(rec, cp);
    rec[4] = adv;
    rec[5] = bpr;
    if (bpr) memcpy(rec + 6, rows, (size_t)bpr * s_height);
    s_io->write(s_path, slot_off(slot), rec, (size_t)s_rec);
}

/* ========================================================================= */
/* Lookup                                                                    */
/* ========================================================================= */

bool mini_font_ignorable(uint32_t cp) {
    return (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x2060 && cp <= 0x2064) || (cp >= 0xFE00 && cp <= 0xFE0F) ||
           cp == 0xFEFF || (cp >= 0xE0000 && cp <= 0xE0FFF) || cp == 0x034F;
}

static bool waiting(uint32_t cp) {
    for (int i = 0; i < s_nwant; i++) if (s_want[i] == cp) return true;
    for (int i = 0; i < s_nfly; i++) if (s_fly[i] == cp) return true;
    return false;
}

/* The cache entry of cp, from the file when needed; asks the server for a
 * code point nothing knows */
static uint8_t *lookup(uint32_t cp) {
    if (cp < 0x20 || cp > 0x10FFFF || mini_font_ignorable(cp) || !store_ready()) return NULL;
    uint8_t *e = cache_find(cp);
    if (e) return e;
    if (waiting(cp)) return NULL;
    e = file_find(cp);
    if (e) return e;
    if (s_nwant < FN_WANT) s_want[s_nwant++] = cp;
    return NULL;
}

const MiniGlyph *mini_fontnet_glyph(uint32_t cp, const MiniFont **used) {
    uint8_t *e = lookup(cp);
    if (!e || e[5] == 0) return NULL;
    MiniGlyph *g = &s_views[s_view];
    s_view = (s_view + 1) % 4;
    g->codepoint = cp;
    g->advance = e[4];
    g->bytes_per_row = e[5];
    g->bitmap = e + 8;
    g->bpp = 1;
    if (used) *used = &s_font;
    return g;
}

int mini_fontnet_advance(uint32_t cp) {
    uint8_t *e = lookup(cp);
    return e && e[5] ? e[4] : -1;
}

bool mini_fontnet_first(void) {
    return s_on && s_first && s_url[0];
}

void mini_fontnet_configure(const char *url, bool on, bool first, const char *face) {
    store_close();
    snprintf(s_url, sizeof(s_url), "%s", url ? url : MINI_FONTNET_DEFAULT_URL);
    size_t n = strlen(s_url);
    while (n > 0 && s_url[n - 1] == '/') s_url[--n] = '\0';
    s_on = on;
    s_first = first;
    snprintf(s_face_cfg, sizeof(s_face_cfg), "%s", face ? face : "");
    s_failures = 0;
    s_retry_at = 0;
    s_error[0] = '\0';
    mini_gui_invalidate(0, mini_gui_get_height());
}

/* ========================================================================= */
/* Requests                                                                  */
/* ========================================================================= */

const char *mini_fontnet_take(void) {
    if (s_nfly > 0 || s_nwant == 0 || !store_ready() || fn_now_ms() < s_retry_at) return NULL;
    int n = snprintf(s_req_url, sizeof(s_req_url), "%s/font/glyphs?face=%s&size=%d&cps=", s_url, s_face, s_height);
    for (int i = 0; i < s_nwant && i < FN_PER_REQUEST && n < (int)sizeof(s_req_url) - 16; i++) {
        n += snprintf(s_req_url + n, sizeof(s_req_url) - (size_t)n, "%s%lx", s_nfly ? "," : "", (unsigned long)s_want[i]);
        s_fly[s_nfly++] = s_want[i];
    }
    memmove(s_want, s_want + s_nfly, (size_t)(s_nwant - s_nfly) * sizeof(s_want[0]));
    s_nwant -= s_nfly;
    s_requests++;
    return s_req_url;
}

static void request_failed(const char *why) {
    snprintf(s_error, sizeof(s_error), "%s", why && why[0] ? why : "request failed");
    s_failures++;
    s_retry_at = fn_now_ms() + (s_failures <= 3 ? 5000 : 60000);
    for (int i = 0; i < s_nfly && s_nwant < FN_WANT; i++) s_want[s_nwant++] = s_fly[i];   /* asked again later */
    s_nfly = 0;
}

void mini_fontnet_deliver(const uint8_t *body, size_t len, int status, const char *error) {
    if (s_nfly == 0) return;
    if (!s_open) {            /* configuration changed meanwhile */
        s_nfly = 0;
        return;
    }
    if (status != 200 || !body) {
        char why[48];
        if (status > 0) snprintf(why, sizeof(why), "HTTP status %d", status);
        request_failed(status > 0 ? why : (error && error[0] ? error : "cannot connect"));
        return;
    }
    if (len < 16 || memcmp(body, "MGLY", 4) != 0 || body[4] != 1 || body[5] != s_height || body[6] != 1) {
        request_failed("not a glyph reply (font_server)");
        return;
    }
    uint32_t hash = get32(body + 8);
    int count = body[12] | (body[13] << 8);
    if (hash != s_hash || s_hash == 0) {   /* other glyphs on the server: start again */
        s_io->remove(s_path);
        s_hash = hash ? hash : 1;
        write_header();
        cache_clear();
    }
    size_t pos = 16;
    int drawn = 0;
    for (int i = 0; i < count && pos + 6 <= len; i++) {
        uint32_t cp = get32(body + pos);
        uint8_t adv = body[pos + 4], bpr = body[pos + 5];
        size_t n = (size_t)bpr * s_height;
        if (pos + 6 + n > len) break;
        const uint8_t *rows = body + pos + 6;
        pos += 6 + n;
        if (bpr > s_max_bpr) bpr = 0;     /* wider than a slot: drawn as missing */
        if (bpr == 0) {
            adv = 0;
            s_absent++;
        } else {
            drawn++;
        }
        file_put(cp, adv, bpr, rows);
        cache_put(cp, adv, bpr, rows);
        s_received++;
    }
    s_nfly = 0;
    s_failures = 0;
    s_retry_at = 0;
    s_error[0] = '\0';
    if (drawn) mini_gui_invalidate(0, mini_gui_get_height());
}

/* ========================================================================= */
/* Scheme                                                                    */
/* ========================================================================= */

static void push_pair(MiniVM *vm, Value *lst, const char *key, Value v) {
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

static const char *state_name(void) {
    return !s_on || !s_url[0] ? "off" : s_nfly ? "fetching" :
           (s_nwant && fn_now_ms() < s_retry_at) ? "waiting" : s_nwant ? "queued" : "idle";
}

const char *mini_fontnet_status(uint32_t *received, uint32_t *absent, const char **error) {
    if (received) *received = s_received;
    if (absent) *absent = s_absent;
    if (error) *error = s_error;
    return state_name();
}

/* (font-server) => ((on . #t) (url . "...") (first . #f) (face . "simsun")
 *   (size . 16) (state . s) (pending . n) (received . n) (absent . n)
 *   (requests . n) (error . "")) */
static Value prim_font_server(MiniVM *vm, int argc, Value *argv) {
    (void)argc; (void)argv;
    bool ready = store_ready();
    const char *state = state_name();
    int height = s_height;
    const char *face = ready ? s_face : (s_face_cfg[0] ? s_face_cfg : mini_font_system_face(&height));
    Value lst = mini_nil();
    mini_push_root(vm, &lst);
    push_pair(vm, &lst, "error", mini_make_string(vm, s_error));
    push_pair(vm, &lst, "requests", mini_make_fixnum((int64_t)s_requests));
    push_pair(vm, &lst, "absent", mini_make_fixnum((int64_t)s_absent));
    push_pair(vm, &lst, "received", mini_make_fixnum((int64_t)s_received));
    push_pair(vm, &lst, "pending", mini_make_fixnum((int64_t)(s_nwant + s_nfly)));
    push_pair(vm, &lst, "state", mini_make_symbol(vm, state));
    push_pair(vm, &lst, "size", mini_make_fixnum((int64_t)height));
    push_pair(vm, &lst, "face", mini_make_string(vm, face));
    push_pair(vm, &lst, "first", s_first ? mini_true() : mini_false());
    push_pair(vm, &lst, "url", mini_make_string(vm, s_url));
    push_pair(vm, &lst, "on", s_on ? mini_true() : mini_false());
    mini_pop_root(vm);
    return lst;
}

/* (font-server-clear) => #t: the downloaded glyphs are dropped (asked again) */
static Value prim_font_server_clear(MiniVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc; (void)argv;
    if (store_ready()) {
        s_io->remove(s_path);
        s_hash = 0;
        cache_clear();
        s_nwant = s_nfly = 0;
    }
    mini_gui_invalidate(0, mini_gui_get_height());
    return mini_true();
}

void mini_register_fontnet_lib(MiniVM *vm) {
    static const MiniPrimEntry funcs[] = {
        { "font-server",       prim_font_server },
        { "font-server-clear", prim_font_server_clear },
    };
    mini_register_prim_table(vm, funcs, sizeof(funcs) / sizeof(funcs[0]));
}
