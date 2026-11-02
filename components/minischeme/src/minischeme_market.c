/**
 * @file minischeme_market.c
 * @brief The app market engine (see minischeme_market.h).
 */

#include "minischeme_market.h"
#include "minischeme_internal.h"
#include "minischeme_device.h"
#include "minischeme_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MARKET_INFO_MAX   2048     /* bytes of one <id>.info.json */

typedef struct {
    char id[MINI_MARKET_ID_MAX], name[48], version[12], api[12], ext[24], file[40], desc[100];
    uint32_t size, hash;
    char path[48];          /* the installed copy, or where an install goes */
    char inst_version[12];  /* "" when not installed */
    uint8_t status;         /* MiniMarketAppStatus */
    uint8_t tries;          /* failed downloads of this install (one retry) */
    bool info;              /* its info was read (or could not be: absent) */
    bool queued;            /* to install */
    bool want;              /* asked for by id: install once its info is here */
} MarketApp;

static const MiniMarketHost *H = NULL;
static MarketApp *s_apps = NULL;       /* malloc'd while a list is held */
static int s_count = 0, s_cap = 0;
static MiniMarketState s_state = MINI_MARKET_IDLE;
static char s_msg[112] = "";
static char s_base[160] = "";          /* the market URL of the list */
static int s_cur = -1;                 /* the app whose info or image is fetched */
static bool s_cur_info = false;        /* ... its info (else its image) */
static size_t s_recv = 0, s_total = 0;
static int s_done = 0, s_failed = 0;   /* installs of the current queue */
static bool s_checking = false;        /* a check round runs (infos of the list) */
static bool s_installing = false;      /* an install round runs */
static int s_absent = 0, s_unread = 0; /* apps of the check the market lacks; infos not read */
static char s_unread_err[64] = "";
static bool s_then_updates = false;    /* after the check: install the updates (update-all) */
static MiniMarketAuto s_auto = MINI_MARKET_AUTO_OFF;   /* the automatic round of this boot */
static bool s_auto_busy = false;       /* its refresh or installs run */
static bool s_auto_t0_set = false;
static uint32_t s_auto_t0 = 0;         /* its first mini_market_auto call */
static bool s_return = false;          /* an update round is over: back to the app */
static char s_auto_app[MINI_MARKET_ID_MAX] = "";   /* the app a boot check looks at */

void mini_market_set_host(const MiniMarketHost *host) { H = host; }

static void say(const char *msg) {
    snprintf(s_msg, sizeof(s_msg), "%s", msg);
    if (H && H->log) H->log(msg);
}

void mini_market_url(char *out, size_t cap) {
    char v[160] = "";
    if (cap == 0) return;
    out[0] = '\0';
    if (!H || !H->config) return;
    if (H->config("market_url", v, sizeof(v)) && v[0]) {
        snprintf(out, cap, "%s", v);
    } else if (H->config("proxy", v, sizeof(v)) && v[0]) {
        size_t n = strlen(v);
        while (n > 0 && v[n - 1] == '/') v[--n] = '\0';
        snprintf(out, cap, "%.150s/market", v);
    }
    size_t n = strlen(out);
    while (n > 0 && out[n - 1] == '/') out[--n] = '\0';
}

/* ------------------------------------------------------------------------- */
/* Numbers and versions (no strtol: the WASM libc has none)                  */
/* ------------------------------------------------------------------------- */

static unsigned long parse_uint(const char **p, int base) {
    unsigned long v = 0;
    for (;; (*p)++) {
        char c = **p;
        int d = c >= '0' && c <= '9' ? c - '0' : (base == 16 && c >= 'a' && c <= 'f') ? c - 'a' + 10 :
                (base == 16 && c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (d < 0) return v;
        v = v * (unsigned long)base + (unsigned long)d;
    }
}

/* "1.10" > "1.9"; missing parts are 0 */
static int version_cmp(const char *a, const char *b) {
    while (*a || *b) {
        unsigned long x = parse_uint(&a, 10), y = parse_uint(&b, 10);
        if (x != y) return x < y ? -1 : 1;
        if (*a == '.') a++; else if (*a) a = "";
        if (*b == '.') b++; else if (*b) b = "";
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* index.json: an array of flat objects                                      */
/* ------------------------------------------------------------------------- */

static const char *skip_ws(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    return p;
}

static void put_utf8(char *out, size_t cap, size_t *n, unsigned cp) {
    char b[4];
    size_t k;
    if (cp < 0x80) { b[0] = (char)cp; k = 1; }
    else if (cp < 0x800) { b[0] = (char)(0xC0 | (cp >> 6)); b[1] = (char)(0x80 | (cp & 0x3F)); k = 2; }
    else { b[0] = (char)(0xE0 | (cp >> 12)); b[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); b[2] = (char)(0x80 | (cp & 0x3F)); k = 3; }
    if (*n + k < cap) { memcpy(out + *n, b, k); *n += k; }
}

/* A JSON string at p (its opening quote) into out (cut to cap); returns the
 * position after it, NULL when malformed */
static const char *parse_string(const char *p, const char *end, char *out, size_t cap) {
    size_t n = 0;
    if (p >= end || *p != '"') return NULL;
    for (p++; p < end && *p != '"'; p++) {
        char c = *p;
        if (c == '\\') {
            if (++p >= end) return NULL;
            switch (*p) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'u': {
                unsigned cp = 0;
                for (int i = 0; i < 4; i++) {
                    if (++p >= end) return NULL;
                    char h = *p;
                    cp = cp * 16 + (unsigned)(h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 :
                                              h >= 'A' && h <= 'F' ? h - 'A' + 10 : 0);
                }
                put_utf8(out, cap, &n, cp >= 0xD800 && cp <= 0xDFFF ? 0xFFFD : cp);
                continue;
            }
            default: c = *p; break;      /* \" \\ \/ */
            }
        }
        if (n + 1 < cap) out[n++] = c;
    }
    if (cap) out[n < cap ? n : cap - 1] = '\0';
    /* a cut string must not end inside a UTF-8 sequence */
    while (n > 0 && n + 1 >= cap && ((unsigned char)out[n - 1] & 0xC0) == 0x80) out[--n] = '\0';
    if (n > 0 && n + 1 >= cap && ((unsigned char)out[n - 1] & 0xC0) == 0xC0) out[--n] = '\0';
    return p < end ? p + 1 : NULL;
}

/* Skips any JSON value; NULL when malformed */
static const char *skip_value(const char *p, const char *end) {
    p = skip_ws(p, end);
    if (p >= end) return NULL;
    if (*p == '"') {
        char tmp[2];
        return parse_string(p, end, tmp, sizeof(tmp));
    }
    if (*p == '{' || *p == '[') {
        int depth = 0;
        for (; p < end; p++) {
            if (*p == '"') {
                char tmp[2];
                p = parse_string(p, end, tmp, sizeof(tmp));
                if (!p) return NULL;
                p--;
            } else if (*p == '{' || *p == '[') {
                depth++;
            } else if (*p == '}' || *p == ']') {
                if (--depth == 0) return p + 1;
            }
        }
        return NULL;
    }
    while (p < end && *p != ',' && *p != '}' && *p != ']') p++;   /* number, true, false, null */
    return p;
}

/* src cut to cap at a UTF-8 character boundary (names are Chinese) */
static void copy_utf8(char *dst, size_t cap, const char *src) {
    size_t n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) n--;   /* src[n] starts the cut character */
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void field(MarketApp *a, const char *key, const char *val) {
    struct { const char *k; char *dst; size_t cap; } f[] = {
        { "id", a->id, sizeof(a->id) }, { "name", a->name, sizeof(a->name) },
        { "version", a->version, sizeof(a->version) }, { "api", a->api, sizeof(a->api) },
        { "extensions", a->ext, sizeof(a->ext) }, { "file", a->file, sizeof(a->file) },
        { "description", a->desc, sizeof(a->desc) },
    };
    for (size_t i = 0; i < sizeof(f) / sizeof(f[0]); i++) {
        if (strcmp(key, f[i].k) == 0) copy_utf8(f[i].dst, f[i].cap, val);
    }
    const char *v = val;
    if (strcmp(key, "size") == 0) a->size = (uint32_t)parse_uint(&v, 10);
    if (strcmp(key, "hash") == 0) a->hash = (uint32_t)parse_uint(&v, 16);
}

/* An app's <id>.info.json: one flat object (index.json's entry of it) into
 * *a; false when malformed, of another app or without a safe image name */
static bool parse_info(const char *p, size_t len, MarketApp *a) {
    const char *end = p + len;
    MarketApp got;
    memset(&got, 0, sizeof(got));
    p = skip_ws(p, end);
    if (p >= end || *p != '{') return false;
    p = skip_ws(p + 1, end);
    while (p < end && *p != '}') {
        char key[16], val[128];
        p = parse_string(p, end, key, sizeof(key));
        if (!p) return false;
        p = skip_ws(p, end);
        if (p >= end || *p != ':') return false;
        p = skip_ws(p + 1, end);
        if (p < end && *p == '"') {
            p = parse_string(p, end, val, sizeof(val));
        } else {
            const char *v = p;
            p = skip_value(p, end);
            if (!p) return false;
            size_t vl = (size_t)(p - v) < sizeof(val) - 1 ? (size_t)(p - v) : sizeof(val) - 1;
            memcpy(val, v, vl);
            val[vl] = '\0';
        }
        if (!p) return false;
        field(&got, key, val);
        p = skip_ws(p, end);
        if (p < end && *p == ',') p = skip_ws(p + 1, end);
    }
    if (p >= end || strcmp(got.id, a->id) != 0 || !got.file[0] || strchr(got.file, '/') || strstr(got.file, "..")) {
        return false;
    }
    memcpy(a->name, got.name, sizeof(a->name));
    memcpy(a->version, got.version, sizeof(a->version));
    memcpy(a->api, got.api, sizeof(a->api));
    memcpy(a->ext, got.ext, sizeof(a->ext));
    memcpy(a->file, got.file, sizeof(a->file));
    memcpy(a->desc, got.desc, sizeof(a->desc));
    a->size = got.size;
    a->hash = got.hash;
    return true;
}

/* ------------------------------------------------------------------------- */
/* Installed copies                                                          */
/* ------------------------------------------------------------------------- */

static void meta_value(const char *meta, const char *key, char *out, size_t cap) {
    if (!meta || !mini_device_config_value(meta, key, out, cap)) out[0] = '\0';
}

static void compare_installed(MarketApp *a) {
    char path[48], meta_id[24];
    size_t size;
    uint32_t hash = 0;
    uint8_t head[4];
    bool have = false;
    a->inst_version[0] = '\0';
    if (!mini_app_id_ok(a->id)) {           /* not a folder name: never installed */
        a->path[0] = '\0';
        a->status = MINI_MARKET_INCOMPATIBLE;
        return;
    }
    snprintf(a->path, sizeof(a->path), "/spiffs/apps/%.23s/app.msbc", a->id);
    snprintf(path, sizeof(path), "%s", a->path);
    have = H->file_hash(path, &size, &hash, head);
    if (!have && H->file_hash("/spiffs/app.msbc", &size, &hash, head)) {
        /* the root app (the firmware's default) when it is this one */
        char *meta = H->file_meta("/spiffs/app.msbc");
        meta_value(meta, "id", meta_id, sizeof(meta_id));
        free(meta);
        if (strcmp(meta_id, a->id) == 0) {
            snprintf(a->path, sizeof(a->path), "/spiffs/app.msbc");
            have = true;
        }
    }
    if (have) {
        char *meta = H->file_meta(a->path);
        meta_value(meta, "version", a->inst_version, sizeof(a->inst_version));
        if (!a->inst_version[0]) snprintf(a->inst_version, sizeof(a->inst_version), "?");
        if (!a->name[0]) {
            char name[48];
            meta_value(meta, "name", name, sizeof(name));
            copy_utf8(a->name, sizeof(a->name), name[0] ? name : a->id);
        }
        free(meta);
    }
    /* a system app built into the firmware: installed as that copy unless the
     * one in /spiffs/apps is at least as new (which then runs instead) */
    size_t blen = 0;
    const uint8_t *bdata = H->builtin ? H->builtin(a->id, &blen) : NULL;
    size_t bmeta_len = 0;
    const char *bmeta = bdata ? mini_app_meta_find(bdata, blen, &bmeta_len) : NULL;
    if (bmeta) {
        char *t = (char *)malloc(bmeta_len + 1);
        char bver[12] = "";
        if (t) {
            memcpy(t, bmeta, bmeta_len);
            t[bmeta_len] = '\0';
            meta_value(t, "version", bver, sizeof(bver));
            free(t);
        }
        if (!have || version_cmp(bver, a->inst_version) > 0) {
            have = true;
            hash = mini_fnv1a(bdata, blen);
            snprintf(a->inst_version, sizeof(a->inst_version), "%s", bver[0] ? bver : "?");
        }
        if (!a->name[0]) copy_utf8(a->name, sizeof(a->name), a->id);
    }
    if (!a->info) {                         /* its info is not read yet */
        a->status = MINI_MARKET_CHECKING;
        return;
    }
    if (!a->file[0]) {                      /* the market has no such app */
        a->status = MINI_MARKET_ABSENT;
        return;
    }
    char need[96];
    snprintf(need, sizeof(need), "[app]\napi = %s\nextensions = %s\n", a->api[0] ? a->api : "1.0", a->ext);
    bool fits = mini_api_check_meta(need, NULL, 0);
    if (have && hash == a->hash) a->status = MINI_MARKET_INSTALLED;
    else if (!fits) a->status = MINI_MARKET_INCOMPATIBLE;
    else if (!have) a->status = MINI_MARKET_NEW;
    else a->status = version_cmp(a->version, a->inst_version) >= 0 ? MINI_MARKET_UPDATE : MINI_MARKET_INSTALLED;
}

/* ------------------------------------------------------------------------- */
/* Refresh and install                                                       */
/* ------------------------------------------------------------------------- */

int mini_market_version_cmp(const char *a, const char *b) { return version_cmp(a, b); }

bool mini_market_use_installed(const char *id, const char *file_meta, const uint8_t *builtin, size_t builtin_len,
                               char *why, size_t cap) {
    char fid[32] = "", fver[12] = "", bver[12] = "", err[96];
    if (why && cap) why[0] = '\0';
    if (!file_meta) {
        if (why) snprintf(why, cap, "not an app image");
        return false;
    }
    meta_value(file_meta, "id", fid, sizeof(fid));
    if (strcmp(fid, id) != 0) {
        if (why) snprintf(why, cap, "its id is %.24s, not %.24s", fid[0] ? fid : "?", id);
        return false;
    }
    if (!mini_api_check_meta(file_meta, err, sizeof(err))) {
        if (why) snprintf(why, cap, "%.90s", err);
        return false;
    }
    meta_value(file_meta, "version", fver, sizeof(fver));
    size_t ml = 0;
    const char *bm = builtin ? mini_app_meta_find(builtin, builtin_len, &ml) : NULL;
    char *t = bm ? (char *)malloc(ml + 1) : NULL;
    if (t) {
        memcpy(t, bm, ml);
        t[ml] = '\0';
        meta_value(t, "version", bver, sizeof(bver));
        free(t);
    }
    /* only a newer one: the same version number is the firmware's own copy (or
     * an older build of it), and the built-in one may have been fixed since */
    if (version_cmp(fver, bver) <= 0) {
        if (why) snprintf(why, cap, "version %.11s is not newer than the built-in %.11s", fver, bver);
        return false;
    }
    return true;
}

static void step(void);
static void auto_refreshed(bool ok, int updates);
static void auto_installed(void);

static bool busy(void) { return s_state == MINI_MARKET_REFRESHING || s_state == MINI_MARKET_INSTALLING; }

/* The market URL now; false (with err) without one or without the network */
static bool market_ready(char *err, size_t cap) {
    if (!H) { if (err) snprintf(err, cap, "market: not available"); return false; }
    mini_market_url(s_base, sizeof(s_base));
    if (!s_base[0]) {
        if (err) snprintf(err, cap, "market: no market_url and no proxy in config.ini");
        return false;
    }
    if (H->online && !H->online()) {
        if (err) snprintf(err, cap, "market: not connected");
        return false;
    }
    return true;
}

static int find_app(const char *id) {
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_apps[i].id, id) == 0) return i;
    }
    return -1;
}

/* An entry for id (its info still to read); -1 when memory is short */
static int add_app(const char *id) {
    if (s_count == s_cap) {
        int cap = s_cap ? s_cap * 2 : 4;
        MarketApp *a = (MarketApp *)realloc(s_apps, sizeof(MarketApp) * (size_t)cap);
        if (!a) return -1;
        s_apps = a;
        s_cap = cap;
    }
    MarketApp *a = &s_apps[s_count];
    memset(a, 0, sizeof(*a));
    copy_utf8(a->id, sizeof(a->id), id);
    compare_installed(a);
    return s_count++;
}

/* The list to check: only (one app), or every app installed here */
static bool load_list(const char *only, char *err, size_t cap) {
    free(s_apps);
    s_apps = NULL;
    s_count = s_cap = 0;
    if (only) return add_app(only) >= 0;
    char (*ids)[MINI_MARKET_ID_MAX] = (char (*)[MINI_MARKET_ID_MAX])malloc(MINI_MARKET_ID_MAX * MINI_MARKET_MAX_LOCAL);
    int n = ids && H->installed ? H->installed(ids, MINI_MARKET_MAX_LOCAL) : 0;
    if (!ids) {
        if (err) snprintf(err, cap, "market: out of memory");
        return false;
    }
    for (int i = 0; i < n; i++) {
        ids[i][MINI_MARKET_ID_MAX - 1] = '\0';
        if (mini_app_id_ok(ids[i]) && find_app(ids[i]) < 0 && add_app(ids[i]) < 0) {
            free(ids);
            if (err) snprintf(err, cap, "market: out of memory");
            return false;
        }
    }
    free(ids);
    return true;
}

/* Checks the list (only: one app, NULL: every installed one) */
static bool start_check(const char *only, char *err, size_t cap) {
    if (err && cap) err[0] = '\0';
    if (busy()) {
        if (err) snprintf(err, cap, "market: busy");
        return false;
    }
    if (!market_ready(err, cap) || !load_list(only, err, cap)) return false;
    s_checking = true;
    s_installing = false;
    s_absent = s_unread = 0;
    s_unread_err[0] = '\0';
    s_state = MINI_MARKET_REFRESHING;
    s_recv = s_total = 0;
    say(only ? "checking the app" : "checking the installed apps");
    step();
    return true;
}

bool mini_market_refresh(char *err, size_t cap) {
    s_then_updates = false;
    return start_check(NULL, err, cap);
}

static int count_updates(void) {
    int n = 0;
    for (int i = 0; i < s_count; i++) n += s_apps[i].status == MINI_MARKET_UPDATE;
    return n;
}

/* Queue one entry; false when it cannot be installed (err set) */
static bool queue_app(MarketApp *a, char *err, size_t cap) {
    if (a->status == MINI_MARKET_INCOMPATIBLE) {
        if (err) snprintf(err, cap, "market: %s needs a newer firmware", a->id);
        return false;
    }
    if (a->status == MINI_MARKET_ABSENT) {
        if (err) snprintf(err, cap, "market: no app %s", a->id);
        return false;
    }
    a->queued = true;
    a->tries = 0;
    return true;
}

int mini_market_install(const char *id, char *err, size_t cap) {
    if (err && cap) err[0] = '\0';
    if (H && H->install_here && !H->install_here()) {
        if (err) snprintf(err, cap, "market: install and update apps in the system settings");
        return 0;
    }
    if (s_state == MINI_MARKET_REFRESHING && s_checking) {
        if (err) snprintf(err, cap, "market: busy");
        return 0;
    }
    int n = 0;
    if (strcmp(id, "updates") == 0) {
        for (int i = 0; i < s_count; i++) {
            MarketApp *a = &s_apps[i];
            if (a->status == MINI_MARKET_UPDATE && !a->queued && i != s_cur && queue_app(a, NULL, 0)) n++;
        }
        if (n == 0 && err) snprintf(err, cap, s_count ? "market: no updates" : "market: refresh the list first");
    } else {
        if (!mini_app_id_ok(id)) {
            if (err) snprintf(err, cap, "market: no app %s", id);
            return 0;
        }
        if (!busy() && !market_ready(err, cap)) return 0;
        int i = find_app(id);
        if (i < 0) {                      /* not in the list: its info first, then the image */
            i = add_app(id);
            if (i < 0) {
                if (err) snprintf(err, cap, "market: out of memory");
                return 0;
            }
            s_apps[i].want = true;
            n = 1;
        } else if (!s_apps[i].info) {
            s_apps[i].want = true;
            n = 1;
        } else if (s_apps[i].queued || i == s_cur) {
            n = 1;
        } else {
            n = queue_app(&s_apps[i], err, cap) ? 1 : 0;
        }
    }
    if (n > 0 && !busy()) {
        s_done = s_failed = 0;
        s_installing = true;
        step();
    } else if (n > 0) {
        s_installing = true;
    }
    return n;
}

int mini_market_update_all(char *err, size_t cap) {
    if (err && cap) err[0] = '\0';
    if (H && H->install_here && !H->install_here()) {
        if (!H->auto_update) {
            if (err) snprintf(err, cap, "market: install and update apps in the system settings");
            return 0;
        }
        say("update all: restarting into the settings");
        H->auto_update();
        return 2;
    }
    if (!start_check(NULL, err, cap)) return 0;
    s_then_updates = true;
    return 1;
}

void mini_market_cancel(void) {
    if (!H) return;
    if (busy()) {
        H->fetch_cancel();
        if (s_state == MINI_MARKET_INSTALLING) H->file_remove(MINI_MARKET_TMP);
    }
    for (int i = 0; i < s_count; i++) s_apps[i].queued = s_apps[i].want = false;
    s_cur = -1;
    s_checking = s_installing = s_then_updates = false;
    s_state = MINI_MARKET_IDLE;
    s_auto = MINI_MARKET_AUTO_OFF;
    s_auto_busy = false;
    say("cancelled");
}

/* The next fetch: an info still to read, else a queued image; else the
 * rounds that ran are over */
static void step(void) {
    char url[240], m[112];
    s_cur = -1;
    for (int i = 0; i < s_count; i++) {
        MarketApp *a = &s_apps[i];
        if (a->info) continue;
        snprintf(url, sizeof(url), "%.159s/%.23s.info.json", s_base, a->id);
        s_recv = s_total = 0;
        if (!H->fetch_start(url, NULL, MARKET_INFO_MAX)) {
            a->info = true;                 /* cannot start: counts as unread */
            s_unread++;
            snprintf(s_unread_err, sizeof(s_unread_err), "cannot fetch %.40s", url);
            compare_installed(a);
            continue;
        }
        s_cur = i;
        s_cur_info = true;
        s_state = MINI_MARKET_REFRESHING;
        return;
    }
    if (s_checking) {                       /* every info is here */
        s_checking = false;
        int updates = count_updates();
        bool none = s_count > 0 && s_unread == s_count;
        if (none) {
            snprintf(m, sizeof(m), "market: %.90s", s_unread_err[0] ? s_unread_err : "no app info could be read");
        } else {
            snprintf(m, sizeof(m), "%d apps, %d updates", s_count, updates);
            if (s_absent) snprintf(m + strlen(m), sizeof(m) - strlen(m), ", %d not in the market", s_absent);
        }
        say(m);
        s_state = none ? MINI_MARKET_ERROR : MINI_MARKET_IDLE;
        if (!none && s_then_updates) {
            s_then_updates = false;
            char err[112];
            if (mini_market_install("updates", err, sizeof(err)) <= 0) say(updates ? err : "no updates");
            if (s_installing) return;       /* the install round has started (step ran) */
        }
        s_then_updates = false;
        auto_refreshed(!none, updates);
        if (busy()) return;
    }
    for (int i = 0; i < s_count; i++) {
        MarketApp *a = &s_apps[i];
        if (!a->queued) continue;
        a->queued = false;
        snprintf(url, sizeof(url), "%.159s/%.39s", s_base, a->file);
        s_recv = 0;
        s_total = a->size;
        if (!H->fetch_start(url, MINI_MARKET_TMP, 0)) {
            snprintf(m, sizeof(m), "%.23s: cannot fetch", a->id);
            say(m);
            s_failed++;
            continue;
        }
        s_cur = i;
        s_cur_info = false;
        s_state = MINI_MARKET_INSTALLING;
        snprintf(m, sizeof(m), "downloading %.23s", a->id);
        say(m);
        return;
    }
    if (s_installing) {                     /* the queue is done */
        s_installing = false;
        snprintf(m, sizeof(m), "%d installed%s", s_done, s_failed ? ", failed: see the log" : "");
        if (!s_failed) say(m);
        s_state = s_failed ? MINI_MARKET_ERROR : MINI_MARKET_IDLE;
        auto_installed();
        return;
    }
    if (s_state != MINI_MARKET_ERROR) s_state = MINI_MARKET_IDLE;
}

/* the downloaded image: complete, unchanged, an app image this firmware runs */
static bool check_download(MarketApp *a, char *why, size_t cap) {
    size_t size = 0;
    uint32_t hash = 0;
    uint8_t head[4] = { 0 };
    if (!H->file_hash(MINI_MARKET_TMP, &size, &hash, head)) {
        snprintf(why, cap, "%.23s: download not saved", a->id);
        return false;
    }
    if ((a->size && size != a->size) || (a->hash && hash != a->hash)) {
        snprintf(why, cap, "%.23s: download damaged (%u bytes, hash %08x; expected %u, %08x)", a->id,
                 (unsigned)size, (unsigned)hash, (unsigned)a->size, (unsigned)a->hash);
        return false;
    }
    if (memcmp(head, "MSBC", 4) != 0 && memcmp(head, "MFAS", 4) != 0) {
        snprintf(why, cap, "%.23s: not an app image", a->id);
        return false;
    }
    char *meta = H->file_meta(MINI_MARKET_TMP);
    char err[96];
    bool ok = mini_api_check_meta(meta, err, sizeof(err));
    free(meta);
    if (!ok) snprintf(why, cap, "%.23s: %.80s", a->id, err);
    return ok;
}

/* the info of s_cur arrived (r 1) or did not (r -1, err) */
static void info_done(int r, const char *err) {
    MarketApp *a = &s_apps[s_cur];
    char m[112];
    bool ok = false;
    if (r > 0) {
        size_t len = 0;
        const char *data = H->fetch_data(&len);
        ok = data && parse_info(data, len, a);
    }
    H->fetch_release();
    a->info = true;
    if (!ok) {
        a->file[0] = '\0';
        /* a 404 is an app the market does not have; anything else could not be read */
        if (r > 0 || (err && strstr(err, "404"))) {
            s_absent++;
        } else {
            s_unread++;
            snprintf(s_unread_err, sizeof(s_unread_err), "%.60s", err ? err : "fetch failed");
        }
    }
    compare_installed(a);
    if (a->want) {
        a->want = false;
        if (!queue_app(a, m, sizeof(m))) {
            if (!ok && r < 0 && !(err && strstr(err, "404"))) snprintf(m, sizeof(m), "%.23s: %.80s", a->id, err ? err : "fetch failed");
            say(m);
            s_failed++;
        }
    }
}

void mini_market_poll(void) {
    if (!H || !busy() || s_cur < 0) return;
    const char *err = NULL;
    size_t total = 0;
    int r = H->fetch_poll(&s_recv, &total, &err);
    if (total) s_total = total;
    if (r == 0) return;
    char m[112];
    if (s_cur_info) {
        info_done(r, err);
        step();
        return;
    }
    /* installing s_cur: a failed download or check is tried once more (the
     * network, or memory short for a moment), then counts as failed */
    MarketApp *a = &s_apps[s_cur];
    bool bad = false;
    if (r < 0) {
        snprintf(m, sizeof(m), "%.23s: %.80s", a->id, err ? err : "download failed");
        bad = true;
    } else if (!check_download(a, m, sizeof(m))) {
        bad = true;
    }
    if (bad) {
        H->file_remove(MINI_MARKET_TMP);
        say(m);
        if (a->tries++ == 0) {
            a->queued = true;         /* again, before the next one */
        } else {
            s_failed++;
        }
    } else if (!H->file_replace(MINI_MARKET_TMP, a->path)) {
        snprintf(m, sizeof(m), "%.23s: cannot write %.47s", a->id, a->path);
        H->file_remove(MINI_MARKET_TMP);
        say(m);
        s_failed++;
    } else {
        snprintf(a->inst_version, sizeof(a->inst_version), "%s", a->version);
        a->status = MINI_MARKET_INSTALLED;
        s_done++;
        snprintf(m, sizeof(m), "%.23s %.11s installed", a->id, a->version);
        say(m);
    }
    /* a queued app first (a retry), in list order */
    step();
}

void mini_market_release(void) {
    if (busy()) return;
    free(s_apps);
    s_apps = NULL;
    s_count = s_cap = 0;
}

/* ------------------------------------------------------------------------- */
/* Automatic updates: one check per app boot, installs in the settings      */
/* ------------------------------------------------------------------------- */

static bool auto_enabled(void) {
    char v[8] = "";
    if (!H || !H->config || !H->config("market_auto_update", v, sizeof(v))) return false;
    return v[0] == '1' || v[0] == 'y' || v[0] == 'Y' || v[0] == 't' || v[0] == 'T' ||
           ((v[0] == 'o' || v[0] == 'O') && (v[1] == 'n' || v[1] == 'N'));
}

static void auto_end(const char *msg) {
    char m[112];
    snprintf(m, sizeof(m), "auto update: %.96s", msg);
    say(m);
    if (s_auto == MINI_MARKET_AUTO_CHECK) {
        mini_market_release();          /* the app's memory: the list is not kept */
    } else if (s_auto == MINI_MARKET_AUTO_UPDATE) {
        s_return = true;
    }
    s_auto = MINI_MARKET_AUTO_OFF;
    s_auto_busy = false;
}

void mini_market_auto_start(MiniMarketAuto mode, const char *app_id) {
    s_auto = mode;
    s_auto_busy = false;
    s_auto_t0_set = false;
    s_return = false;
    copy_utf8(s_auto_app, sizeof(s_auto_app), app_id ? app_id : "");
    /* a boot check looks at the running app alone, and only one the market keeps */
    if (mode == MINI_MARKET_AUTO_CHECK && (!auto_enabled() || !mini_app_id_ok(s_auto_app))) s_auto = MINI_MARKET_AUTO_OFF;
    if (s_auto == MINI_MARKET_AUTO_UPDATE) say("auto update: waiting for the network");
}

MiniMarketAuto mini_market_auto_mode(void) { return s_auto; }

bool mini_market_return(void) { return s_return; }

void mini_market_auto(uint32_t now_ms) {
    if (s_auto == MINI_MARKET_AUTO_OFF || !H || s_auto_busy) return;
    if (!s_auto_t0_set) {
        s_auto_t0 = now_ms;
        s_auto_t0_set = true;
    }
    if (H->online && !H->online()) {
        /* no network at boot: skipped until the next boot */
        if (now_ms - s_auto_t0 >= MINI_MARKET_AUTO_WAIT_MS) auto_end("no network at boot, skipped");
        return;
    }
    char err[112];
    if (s_state == MINI_MARKET_REFRESHING && s_checking) {   /* someone else's check: use it */
        s_auto_busy = true;
        return;
    }
    s_then_updates = false;
    s_auto_busy = true;           /* before: a check that ends at once reports to this round */
    if (!start_check(s_auto == MINI_MARKET_AUTO_CHECK ? s_auto_app : NULL, err, sizeof(err))) {
        s_auto_busy = false;
        auto_end(err);
    }
}

/* the check of the round ended: the infos arrived (ok) or none could be read */
static void auto_refreshed(bool ok, int updates) {
    if (s_auto == MINI_MARKET_AUTO_OFF || !s_auto_busy) return;
    char m[112];
    if (!ok) {
        snprintf(m, sizeof(m), "check failed (%.80s), skipped", s_msg);
        auto_end(m);
        return;
    }
    if (s_auto == MINI_MARKET_AUTO_CHECK) {
        if (updates == 0) {
            auto_end("no updates");
            return;
        }
        snprintf(m, sizeof(m), "auto update: %.23s has an update, restarting into the settings", s_auto_app);
        s_auto = MINI_MARKET_AUTO_OFF;
        s_auto_busy = false;
        if (H->auto_update) {
            say(m);
            H->auto_update();
        } else {
            mini_market_release();
        }
        return;
    }
    char err[112];
    if (mini_market_install("updates", err, sizeof(err)) <= 0) auto_end(err);
}

/* the installs of the round are done */
static void auto_installed(void) {
    if (s_auto != MINI_MARKET_AUTO_UPDATE || !s_auto_busy) return;
    char m[96];
    snprintf(m, sizeof(m), "%d installed, %d failed", s_done, s_failed);
    auto_end(m);
}

MiniMarketState mini_market_state(void) { return s_state; }
const char *mini_market_message(void) { return s_msg; }

/* ------------------------------------------------------------------------- */
/* JSON (GET /api/market)                                                    */
/* ------------------------------------------------------------------------- */

static const char *const STATE_NAMES[] = { "idle", "refreshing", "installing", "error" };
static const char *const STATUS_NAMES[] = { "new", "installed", "update", "incompatible", "checking", "absent" };

static void json_str(char *out, size_t cap, size_t *n, const char *s) {
    if (*n + 1 < cap) out[(*n)++] = '"';
    for (; *s && *n + 7 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[(*n)++] = '\\'; out[(*n)++] = (char)c; }
        else if (c < 0x20) *n += (size_t)snprintf(out + *n, cap - *n, "\\u%04x", c);
        else out[(*n)++] = (char)c;
    }
    if (*n + 1 < cap) out[(*n)++] = '"';
    out[*n < cap ? *n : cap - 1] = '\0';
}

#define JSON_PUT(...) do { if (n < cap) n += (size_t)snprintf(out + n, cap - n, __VA_ARGS__); } while (0)

size_t mini_market_json(char *out, size_t cap) {
    size_t n = 0;
    char url[160];
    if (cap == 0) return 0;
    out[0] = '\0';
    mini_market_poll();
    mini_market_url(url, sizeof(url));
    JSON_PUT("{\"success\":true,\"state\":\"%s\",\"message\":", STATE_NAMES[s_state]);
    json_str(out, cap, &n, s_msg);
    JSON_PUT(",\"url\":");
    json_str(out, cap, &n, url);
    JSON_PUT(",\"current\":");
    if (s_state == MINI_MARKET_INSTALLING && s_cur >= 0) json_str(out, cap, &n, s_apps[s_cur].id); else JSON_PUT("null");
    JSON_PUT(",\"checking\":%s", s_checking ? "true" : "false");
    int queued = 0;
    for (int i = 0; i < s_count; i++) queued += s_apps[i].queued;
    static const char *const AUTO_NAMES[] = { "off", "check", "update" };
    JSON_PUT(",\"received\":%u,\"total\":%u,\"queued\":%d,\"install\":%s,\"auto\":\"%s\",\"return\":%s,\"apps\":[",
             (unsigned)s_recv, (unsigned)s_total, queued, (!H || !H->install_here || H->install_here()) ? "true" : "false",
             AUTO_NAMES[s_auto], s_return ? "true" : "false");
    for (int i = 0; i < s_count && n < cap; i++) {
        MarketApp *a = &s_apps[i];
        JSON_PUT("%s{\"id\":", i ? "," : "");
        json_str(out, cap, &n, a->id);
        JSON_PUT(",\"name\":");
        json_str(out, cap, &n, a->name);
        JSON_PUT(",\"description\":");
        json_str(out, cap, &n, a->desc);
        JSON_PUT(",\"version\":");
        json_str(out, cap, &n, a->version);
        JSON_PUT(",\"installed\":");
        json_str(out, cap, &n, a->inst_version);
        JSON_PUT(",\"api\":");
        json_str(out, cap, &n, a->api);
        JSON_PUT(",\"size\":%u,\"status\":\"%s\",\"queued\":%s}", (unsigned)a->size, STATUS_NAMES[a->status],
                 a->queued || a->want || (i == s_cur && !s_cur_info) ? "true" : "false");
    }
    JSON_PUT("]}");
    if (n >= cap) n = cap - 1;
    return n;
}

/* ------------------------------------------------------------------------- */
/* Scheme procedures                                                         */
/* ------------------------------------------------------------------------- */

static Value cons_pair(MiniVM *vm, const char *key, Value v, Value tail) {
    mini_push_root(vm, &v);
    mini_push_root(vm, &tail);
    Value k = mini_make_symbol(vm, key);
    mini_push_root(vm, &k);
    Value cell = mini_make_cons(vm, k, v);
    mini_push_root(vm, &cell);
    Value res = mini_make_cons(vm, cell, tail);
    mini_pop_root(vm);
    mini_pop_root(vm);
    mini_pop_root(vm);
    mini_pop_root(vm);
    return res;
}

/* (market-refresh) => #t when the check of the installed apps started, else a string saying why */
static Value prim_market_refresh(MiniVM *vm, int argc, Value *argv) {
    (void)argc; (void)argv;
    char err[112];
    mini_market_poll();
    return mini_market_refresh(err, sizeof(err)) ? mini_true() : mini_make_string(vm, err);
}

/* (market-install id) / (market-install 'updates) => the number queued, or a string saying why */
static Value prim_market_install(MiniVM *vm, int argc, Value *argv) {
    if (argc != 1 || !(mini_is_string(argv[0]) || mini_is_symbol(argv[0]))) {
        vm_error(vm, "market-install: expected an app id or 'updates");
        return mini_void();
    }
    char id[32], err[112];
    snprintf(id, sizeof(id), "%s", mini_is_symbol(argv[0]) ? mini_symbol_name(argv[0]) : mini_string_data(argv[0]));
    mini_market_poll();
    int n = mini_market_install(id, err, sizeof(err));
    return n > 0 ? mini_make_fixnum(n) : mini_make_string(vm, err);
}

static Value prim_market_cancel(MiniVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc; (void)argv;
    mini_market_cancel();
    return mini_true();
}

/* (market-status) => ((state . idle) (message . "...") (app . "id"/#f) (received . n) (total . n) (queued . n)
 *                     (install . #t/#f) (auto . off/check/update) (return . #t/#f) (url . "...")) */
static Value prim_market_status(MiniVM *vm, int argc, Value *argv) {
    (void)argc; (void)argv;
    char url[160];
    mini_market_poll();
    mini_market_url(url, sizeof(url));
    int queued = 0;
    for (int i = 0; i < s_count; i++) queued += s_apps[i].queued;
    Value l = mini_nil(), v = mini_void();
    mini_push_root(vm, &l);
    mini_push_root(vm, &v);
    v = mini_make_string(vm, url);
    l = cons_pair(vm, "url", v, l);
    l = cons_pair(vm, "return", s_return ? mini_true() : mini_false(), l);
    v = mini_make_symbol(vm, s_auto == MINI_MARKET_AUTO_CHECK ? "check" : s_auto == MINI_MARKET_AUTO_UPDATE ? "update" : "off");
    l = cons_pair(vm, "auto", v, l);
    l = cons_pair(vm, "install", (!H || !H->install_here || H->install_here()) ? mini_true() : mini_false(), l);
    l = cons_pair(vm, "queued", mini_make_fixnum(queued), l);
    l = cons_pair(vm, "total", mini_make_fixnum((int64_t)s_total), l);
    l = cons_pair(vm, "received", mini_make_fixnum((int64_t)s_recv), l);
    v = (s_state == MINI_MARKET_INSTALLING && s_cur >= 0) ? mini_make_string(vm, s_apps[s_cur].id) : mini_false();
    l = cons_pair(vm, "app", v, l);
    v = mini_make_string(vm, s_msg);
    l = cons_pair(vm, "message", v, l);
    v = mini_make_symbol(vm, STATE_NAMES[s_state]);
    l = cons_pair(vm, "state", v, l);
    mini_pop_root(vm);
    mini_pop_root(vm);
    return l;
}

/* (market-apps) => ((id name version installed-version status size description) ...): the installed
 * apps of the last check (status checking until its info is read, absent when the market lacks it) */
static Value prim_market_apps(MiniVM *vm, int argc, Value *argv) {
    (void)argc; (void)argv;
    mini_market_poll();
    Value l = mini_nil(), e = mini_nil(), v = mini_void();
    mini_push_root(vm, &l);
    mini_push_root(vm, &e);
    mini_push_root(vm, &v);
    for (int i = s_count - 1; i >= 0; i--) {
        MarketApp *a = &s_apps[i];
        e = mini_nil();
        v = mini_make_string(vm, a->desc);
        e = mini_make_cons(vm, v, e);
        e = mini_make_cons(vm, mini_make_fixnum((int64_t)a->size), e);
        v = mini_make_symbol(vm, STATUS_NAMES[a->status]);
        e = mini_make_cons(vm, v, e);
        v = a->inst_version[0] ? mini_make_string(vm, a->inst_version) : mini_false();
        e = mini_make_cons(vm, v, e);
        v = mini_make_string(vm, a->version);
        e = mini_make_cons(vm, v, e);
        v = mini_make_string(vm, a->name);
        e = mini_make_cons(vm, v, e);
        v = mini_make_string(vm, a->id);
        e = mini_make_cons(vm, v, e);
        l = mini_make_cons(vm, e, l);
    }
    mini_pop_root(vm);
    mini_pop_root(vm);
    mini_pop_root(vm);
    return l;
}

static const MiniPrimEntry MARKET_PRIMS[] = {
    { "market-refresh", prim_market_refresh },
    { "market-status", prim_market_status },
    { "market-apps", prim_market_apps },
    { "market-install", prim_market_install },
    { "market-cancel", prim_market_cancel },
};

void mini_register_market_lib(MiniVM *vm) {
    mini_register_prim_table(vm, MARKET_PRIMS, sizeof(MARKET_PRIMS) / sizeof(MARKET_PRIMS[0]));
}
