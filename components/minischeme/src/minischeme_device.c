/**
 * @file minischeme_device.c
 * @brief Shared device runtime: clock formatting, lunar calendar, 1 Hz tick.
 */

#include "minischeme_device.h"
#include "minischeme_internal.h"
#include "minischeme_gui.h"
#include "minischeme_fonts.h"
#include "minischeme_fontnet.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/* Lunar year info 1900..2100. Bits 0-3: leap month (0 = none); bits 4-15:
 * month 12..1 big (30 days) flags; bit 16: leap month is big. */
static const uint32_t s_lunar_info[201] = {
    0x04BD8, 0x04AE0, 0x0A570, 0x054D5, 0x0D260, 0x0D950, 0x16554, 0x056A0, 0x09AD0, 0x055D2,  /* 1900 */
    0x04AE0, 0x0A5B6, 0x0A4D0, 0x0D250, 0x1D255, 0x0B540, 0x0D6A0, 0x0ADA2, 0x095B0, 0x14977,  /* 1910 */
    0x04970, 0x0A4B0, 0x0B4B5, 0x06A50, 0x06D40, 0x1AB54, 0x02B60, 0x09570, 0x052F2, 0x04970,  /* 1920 */
    0x06566, 0x0D4A0, 0x0EA50, 0x16A95, 0x05AD0, 0x02B60, 0x186E3, 0x092E0, 0x1C8D7, 0x0C950,  /* 1930 */
    0x0D4A0, 0x1D8A6, 0x0B550, 0x056A0, 0x1A5B4, 0x025D0, 0x092D0, 0x0D2B2, 0x0A950, 0x0B557,  /* 1940 */
    0x06CA0, 0x0B550, 0x15355, 0x04DA0, 0x0A5B0, 0x14573, 0x052B0, 0x0A9A8, 0x0E950, 0x06AA0,  /* 1950 */
    0x0AEA6, 0x0AB50, 0x04B60, 0x0AAE4, 0x0A570, 0x05260, 0x0F263, 0x0D950, 0x05B57, 0x056A0,  /* 1960 */
    0x096D0, 0x04DD5, 0x04AD0, 0x0A4D0, 0x0D4D4, 0x0D250, 0x0D558, 0x0B540, 0x0B6A0, 0x195A6,  /* 1970 */
    0x095B0, 0x049B0, 0x0A974, 0x0A4B0, 0x0B27A, 0x06A50, 0x06D40, 0x0AF46, 0x0AB60, 0x09570,  /* 1980 */
    0x04AF5, 0x04970, 0x064B0, 0x074A3, 0x0EA50, 0x06B58, 0x05AC0, 0x0AB60, 0x096D5, 0x092E0,  /* 1990 */
    0x0C960, 0x0D954, 0x0D4A0, 0x0DA50, 0x07552, 0x056A0, 0x0ABB7, 0x025D0, 0x092D0, 0x0CAB5,  /* 2000 */
    0x0A950, 0x0B4A0, 0x0BAA4, 0x0AD50, 0x055D9, 0x04BA0, 0x0A5B0, 0x15176, 0x052B0, 0x0A930,  /* 2010 */
    0x07954, 0x06AA0, 0x0AD50, 0x05B52, 0x04B60, 0x0A6E6, 0x0A4E0, 0x0D260, 0x0EA65, 0x0D530,  /* 2020 */
    0x05AA0, 0x076A3, 0x096D0, 0x04AFB, 0x04AD0, 0x0A4D0, 0x1D0B6, 0x0D250, 0x0D520, 0x0DD45,  /* 2030 */
    0x0B5A0, 0x056D0, 0x055B2, 0x049B0, 0x0A577, 0x0A4B0, 0x0AA50, 0x1B255, 0x06D20, 0x0ADA0,  /* 2040 */
    0x14B63, 0x09370, 0x049F8, 0x04970, 0x064B0, 0x168A6, 0x0EA50, 0x06B20, 0x1A6C4, 0x0AAE0,  /* 2050 */
    0x092E0, 0x0D2E3, 0x0C960, 0x0D557, 0x0D4A0, 0x0DA50, 0x05D55, 0x056A0, 0x0A6D0, 0x055D4,  /* 2060 */
    0x052D0, 0x0A9B8, 0x0A950, 0x0B4A0, 0x0B6A6, 0x0AD50, 0x055A0, 0x0ABA4, 0x0A5B0, 0x052B0,  /* 2070 */
    0x0B273, 0x06930, 0x07337, 0x06AA0, 0x0AD50, 0x14B55, 0x04B60, 0x0A570, 0x054E4, 0x0D160,  /* 2080 */
    0x0E968, 0x0D520, 0x0DAA0, 0x16AA6, 0x056D0, 0x04AE0, 0x0A9D4, 0x0A2D0, 0x0D150, 0x0F252,  /* 2090 */
    0x0D520,  /* 2100 */
};

static int lunar_year_days(int y) {
    uint32_t info = s_lunar_info[y - 1900];
    int days = 348;
    for (uint32_t bit = 0x8000; bit > 0x8; bit >>= 1) {
        if (info & bit) days++;
    }
    if (info & 0xF) days += (info & 0x10000) ? 30 : 29;
    return days;
}

static int lunar_month_days(int y, int m) {
    return (s_lunar_info[y - 1900] & (0x10000u >> m)) ? 30 : 29;
}

/* Days since 1900-01-01 for a Gregorian date */
static long days_from_1900(int y, int m, int d) {
    static const int cum[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    long days = 0;
    for (int yy = 1900; yy < y; yy++) {
        bool leap = (yy % 4 == 0 && yy % 100 != 0) || (yy % 400 == 0);
        days += leap ? 366 : 365;
    }
    bool leap = (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
    days += cum[m - 1] + (m > 2 && leap ? 1 : 0) + (d - 1);
    return days;
}

bool mini_lunar_from_solar(int year, int month, int day,
                           int *out_lmonth, int *out_lday, bool *out_leap) {
    if (year < 1900 || year > 2100 || month < 1 || month > 12 || day < 1 || day > 31) return false;
    /* Lunar 1900-01-01 fell on Gregorian 1900-01-31 */
    long off = days_from_1900(year, month, day) - 30;
    if (off < 0) return false;
    int y = 1900;
    while (y <= 2100 && off >= lunar_year_days(y)) {
        off -= lunar_year_days(y);
        y++;
    }
    if (y > 2100) return false;
    int leap_month = (int)(s_lunar_info[y - 1900] & 0xF);
    int leap_days = leap_month ? ((s_lunar_info[y - 1900] & 0x10000) ? 30 : 29) : 0;
    int m = 1;
    bool in_leap = false;
    for (;;) {
        int n = in_leap ? leap_days : lunar_month_days(y, m);
        if (off < n) break;
        off -= n;
        if (leap_month == m && !in_leap) {
            in_leap = true;
        } else {
            in_leap = false;
            m++;
        }
    }
    if (out_lmonth) *out_lmonth = m;
    if (out_lday) *out_lday = (int)off + 1;
    if (out_leap) *out_leap = in_leap;
    return true;
}

bool mini_lunar_format(int year, int month, int day, char *buf, size_t len) {
    static const char *const months[] = {
        "正", "二", "三", "四", "五", "六", "七", "八", "九", "十", "冬", "腊"
    };
    static const char *const digits[] = {
        "", "一", "二", "三", "四", "五", "六", "七", "八", "九", "十"
    };
    int lm = 0, ld = 0;
    bool leap = false;
    if (!buf || len == 0) return false;
    buf[0] = '\0';
    if (!mini_lunar_from_solar(year, month, day, &lm, &ld, &leap)) return false;

    char day_str[16];
    if (ld <= 10) {
        snprintf(day_str, sizeof(day_str), "初%s", digits[ld]);
    } else if (ld < 20) {
        snprintf(day_str, sizeof(day_str), "十%s", digits[ld - 10]);
    } else if (ld == 20) {
        snprintf(day_str, sizeof(day_str), "二十");
    } else if (ld < 30) {
        snprintf(day_str, sizeof(day_str), "廿%s", digits[ld - 20]);
    } else {
        snprintf(day_str, sizeof(day_str), "三十");
    }
    snprintf(buf, len, "农历%s%s月%s", leap ? "闰" : "", months[lm - 1], day_str);
    return true;
}

Value mini_device_call_hook(MiniVM *vm, const char *hook, const char *fallback, int argc, Value *argv) {
    Value fn = env_lookup_name(vm, hook);
    if (mini_is_closure(fn) || mini_is_cfunc(fn)) return mini_call(vm, fn, argc, argv);
    return fallback ? mini_call_named(vm, fallback, argc, argv) : mini_void();
}

bool mini_device_has_hook(MiniVM *vm, const char *hook) {
    Value fn = env_lookup_name(vm, hook);
    return mini_is_closure(fn) || mini_is_cfunc(fn);
}

void mini_device_resume_app(MiniVM *vm) {
    mini_font_restore(vm);
    Value fn = env_lookup_name(vm, "app-resume");
    if (mini_is_closure(fn) || mini_is_cfunc(fn)) mini_call(vm, fn, 0, NULL);
}

/* ------------------------------------------------------------------------- */
/* App metadata trailer                                                      */
/* ------------------------------------------------------------------------- */

uint32_t mini_fnv1a(const uint8_t *data, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= data[i];
        h *= 16777619u;
    }
    return h;
}

static uint32_t rd32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

const char *mini_app_meta_find(const uint8_t *data, size_t len, size_t *meta_len) {
    if (!data || len < MINI_APP_META_FOOTER + 8) return NULL;
    const uint8_t *f = data + len - MINI_APP_META_FOOTER;
    if (memcmp(f + 8, "MSMD", 4) != 0) return NULL;
    uint32_t n = rd32le(f);
    if (n == 0 || n > MINI_APP_META_MAX || n > len - MINI_APP_META_FOOTER - 8) return NULL;
    const uint8_t *meta = f - n;
    if (mini_fnv1a(meta, n) != rd32le(f + 4)) return NULL;
    if (meta_len) *meta_len = n;
    return (const char *)meta;
}

char *mini_app_meta_read_file(const char *path) {
    FILE *fp = path ? fopen(path, "rb") : NULL;
    if (!fp) return NULL;
    uint8_t foot[MINI_APP_META_FOOTER];
    char *text = NULL;
    long size = -1;
    if (fseek(fp, 0, SEEK_END) == 0) size = ftell(fp);
    if (size >= MINI_APP_META_FOOTER + 8 && fseek(fp, size - MINI_APP_META_FOOTER, SEEK_SET) == 0 &&
        fread(foot, 1, sizeof(foot), fp) == sizeof(foot) && memcmp(foot + 8, "MSMD", 4) == 0) {
        uint32_t n = rd32le(foot);
        if (n > 0 && n <= MINI_APP_META_MAX && (long)n <= size - MINI_APP_META_FOOTER - 8 &&
            fseek(fp, size - MINI_APP_META_FOOTER - (long)n, SEEK_SET) == 0 && (text = (char *)malloc(n + 1)) != NULL) {
            if (fread(text, 1, n, fp) != n || mini_fnv1a((const uint8_t *)text, n) != rd32le(foot + 4)) {
                free(text);
                text = NULL;
            } else {
                text[n] = '\0';
            }
        }
    }
    fclose(fp);
    return text;
}

const uint8_t *mini_app_web_find(const uint8_t *data, size_t len, size_t *web_len) {
    size_t meta_len = 0;
    const char *meta = mini_app_meta_find(data, len, &meta_len);
    if (!meta || (size_t)((const uint8_t *)meta - data) < MINI_APP_WEB_FOOTER) return NULL;
    const uint8_t *f = (const uint8_t *)meta - MINI_APP_WEB_FOOTER;
    if (memcmp(f + 8, "MSWP", 4) != 0) return NULL;
    uint32_t n = rd32le(f);
    if (n == 0 || n > (size_t)(f - data)) return NULL;
    const uint8_t *page = f - n;
    if (mini_fnv1a(page, n) != rd32le(f + 4)) return NULL;
    if (web_len) *web_len = n;
    return page;
}

bool mini_app_web_locate_file(const char *path, long *offset, size_t *web_len) {
    FILE *fp = path ? fopen(path, "rb") : NULL;
    if (!fp) return false;
    uint8_t foot[MINI_APP_META_FOOTER];
    long size = -1;
    bool ok = false;
    if (fseek(fp, 0, SEEK_END) == 0) size = ftell(fp);
    if (size >= MINI_APP_META_FOOTER + MINI_APP_WEB_FOOTER + 8 && fseek(fp, size - MINI_APP_META_FOOTER, SEEK_SET) == 0 &&
        fread(foot, 1, sizeof(foot), fp) == sizeof(foot) && memcmp(foot + 8, "MSMD", 4) == 0) {
        long meta_at = size - MINI_APP_META_FOOTER - (long)rd32le(foot);
        uint8_t wf[MINI_APP_WEB_FOOTER];
        if (meta_at >= MINI_APP_WEB_FOOTER && fseek(fp, meta_at - MINI_APP_WEB_FOOTER, SEEK_SET) == 0 &&
            fread(wf, 1, sizeof(wf), fp) == sizeof(wf) && memcmp(wf + 8, "MSWP", 4) == 0) {
            uint32_t n = rd32le(wf);
            long at = meta_at - MINI_APP_WEB_FOOTER - (long)n;
            if (n > 0 && at >= 4) {
                if (offset) *offset = at;
                if (web_len) *web_len = n;
                ok = true;
            }
        }
    }
    fclose(fp);
    return ok;
}

const char *mini_app_meta_config(const char *meta) {
    for (const char *line = meta; line && *line; ) {
        const char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "[config]", 8) == 0) return line;
        line = strchr(line, '\n');
        if (line) line++;
    }
    return NULL;
}

char *mini_app_ini_missing(const char *ini, const char *defaults) {
    size_t cap = (defaults ? strlen(defaults) : 0) + 2, used = 0;  /* + a last line's newline */
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    out[0] = '\0';
    bool in_config = false;
    for (const char *line = defaults; line && *line; ) {
        const char *end = strchr(line, '\n');
        if (!end) end = line + strlen(line);
        const char *p = line;
        while (p < end && (*p == ' ' || *p == '\t')) p++;
        if (p < end && *p == '[') {               /* [config], and the end of it at the next section */
            if (in_config) break;
            in_config = true;
        } else if (p < end && *p != ';' && *p != '#') {
            const char *eq = p;
            while (eq < end && *eq != '=' && *eq != ';' && *eq != '#') eq++;
            const char *k_end = eq;
            while (k_end > p && (k_end[-1] == ' ' || k_end[-1] == '\t')) k_end--;
            char key[64], val[2];
            size_t kn = (size_t)(k_end - p);
            if (eq < end && *eq == '=' && kn > 0 && kn < sizeof(key)) {
                memcpy(key, p, kn);
                key[kn] = '\0';
                if (!ini || !mini_device_config_value(ini, key, val, sizeof(val))) {
                    size_t n = (size_t)(end - line);
                    if (n && line[n - 1] == '\r') n--;
                    memcpy(out + used, line, n);   /* fits: out is as long as defaults */
                    used += n;
                    out[used++] = '\n';
                    out[used] = '\0';
                }
            }
        }
        line = *end ? end + 1 : end;
    }
    return out;
}

bool mini_app_id_ok(const char *id) {
    size_t n = 0;
    if (!id) return false;
    for (; id[n]; n++) {
        char c = id[n];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return false;
        }
    }
    return n >= 1 && n <= 31;
}

/* "/spiffs/apps/<x>/..." or "/spiffs/user/<x>/...": the folder's <x> into out */
static bool folder_app(const char *path, char *out, size_t cap) {
    const char *rest = NULL;
    if (strncmp(path, MINI_APPS_DIR "/", sizeof(MINI_APPS_DIR)) == 0) rest = path + sizeof(MINI_APPS_DIR);
    else if (strncmp(path, MINI_USER_DIR "/", sizeof(MINI_USER_DIR)) == 0) rest = path + sizeof(MINI_USER_DIR);
    if (!rest) return false;
    const char *slash = strchr(rest, '/');
    size_t n = slash ? (size_t)(slash - rest) : 0;
    if (!slash || n == 0 || n >= cap) return false;
    memcpy(out, rest, n);
    out[n] = '\0';
    return mini_app_id_ok(out);
}

void mini_app_id(const char *path, const char *meta, char *out, size_t cap) {
    if (cap == 0) return;
    out[0] = '\0';
    if (meta && mini_device_config_value(meta, "id", out, cap) && mini_app_id_ok(out)) return;
    if (folder_app(path, out, cap)) return;            /* apps/<id>/app.msbc */
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    size_t n = 0;
    while (base[n] && base[n] != '.') n++; /* no strcspn in the WASM libc */
    if (n >= cap) n = cap - 1;
    memcpy(out, base, n);
    out[n] = '\0';
    if (!mini_app_id_ok(out)) snprintf(out, cap, "app");
}

void mini_app_dir_of(const char *path, const char *id, char *out, size_t cap) {
    char x[32];
    if (folder_app(path, x, sizeof(x))) {
        const char *slash = strrchr(path, '/');
        size_t n = (size_t)(slash - path);
        if (n >= cap) n = cap - 1;
        memcpy(out, path, n);
        out[n] = '\0';
        return;
    }
    snprintf(out, cap, MINI_APPS_DIR "/%.31s", mini_app_id_ok(id) ? id : "app");
}

const char *mini_app_source(const char *path) {
    char x[32];
    if (!folder_app(path, x, sizeof(x))) return "builtin";
    return strncmp(path, MINI_USER_DIR "/", sizeof(MINI_USER_DIR)) == 0 ? "user" : "market";
}

void mini_app_key(const char *path, char *out, size_t cap) {
    char x[32];
    if (!folder_app(path, x, sizeof(x))) snprintf(out, cap, "builtin");
    else snprintf(out, cap, "%s/%s", strcmp(mini_app_source(path), "user") == 0 ? "user" : "apps", x);
}

bool mini_app_key_dir(const char *key, char *out, size_t cap) {
    const char *id = NULL;
    if (!key) return false;
    if (strncmp(key, "apps/", 5) == 0) id = key + 5;
    else if (strncmp(key, "user/", 5) == 0) id = key + 5;
    if (!id || !mini_app_id_ok(id)) return false;
    return snprintf(out, cap, "/spiffs/%s", key) < (int)cap;
}

bool mini_app_legacy_name(const char *name, char *out, size_t cap) {
    char id[32];
    size_t n = 0;
    while (name[n] && name[n] != '.' && n < sizeof(id) - 1) n++;
    if (name[n] != '.') return false;
    memcpy(id, name, n);
    id[n] = '\0';
    if (!mini_app_id_ok(id) || strchr(name, '/')) return false;
    const char *ext = name + n;
    int w;
    if (strcmp(ext, ".msbc") == 0 || strcmp(ext, ".fasl") == 0) w = snprintf(out, cap, "%s/app%s", id, ext);
    else if (strcmp(ext, ".ini") == 0) w = snprintf(out, cap, "%s/config.ini", id);
    else w = snprintf(out, cap, "%s/%s", id, name);
    return w > 0 && w < (int)cap;
}

bool mini_app_legacy_boot(const char *value, char *out, size_t cap) {
    const char *rel = strncmp(value, "/spiffs/", 8) == 0 ? value + 8 : (value[0] == '/' ? value + 1 : value);
    char moved[64];
    if (strncmp(rel, "apps/", 5) != 0 || strchr(rel + 5, '/') || !mini_app_legacy_name(rel + 5, moved, sizeof(moved))) {
        return false;
    }
    return snprintf(out, cap, "apps/%s", moved) < (int)cap;
}

/* ---- the sandbox ---------------------------------------------------------- */

static char s_box[64] = "";        /* the running app's folder, "" none */
static int s_priv = 0;              /* privileged sections running (REPL jobs) */
static size_t s_quota = 256 * 1024;
static size_t (*s_dir_used)(const char *dir) = NULL;

bool mini_app_sandboxed(void) { return s_box[0] && s_priv == 0; }
const char *mini_app_sandbox_dir(void) { return s_box; }
void mini_app_privileged_begin(void) { s_priv++; }
void mini_app_privileged_end(void) { if (s_priv > 0) s_priv--; }

void mini_app_set_storage(size_t quota, size_t (*dir_used)(const char *dir)) {
    s_quota = quota;
    s_dir_used = dir_used;
}

bool mini_app_storage_ok(size_t bytes, size_t old, char *why, size_t cap) {
    if (!mini_app_sandboxed() || !s_dir_used) return true;
    size_t used = s_dir_used(s_box);
    used = used > old ? used - old : 0;
    if (used + bytes <= s_quota) return true;
    if (why) snprintf(why, cap, "the app's storage is full (%u of %u KB)", (unsigned)((used + 1023) / 1024),
                      (unsigned)(s_quota / 1024));
    return false;
}

size_t mini_app_storage_room(void) {
    if (!mini_app_sandboxed() || !s_dir_used) return (size_t)-1;
    size_t used = s_dir_used(s_box);
    return used < s_quota ? s_quota - used : 0;
}

/* No "..", backslashes, empty parts or control characters */
static bool path_clean(const char *p) {
    for (const char *c = p; *c; c++) {
        if ((unsigned char)*c < 32 || *c == '\\') return false;
        if (*c == '/' && c[1] == '/') return false;
        if (*c == '.' && c[1] == '.' && (c == p || c[-1] == '/') && (c[2] == '/' || c[2] == '\0')) return false;
    }
    return true;
}

static bool box_file(const char *path, bool write, char *out, size_t cap, char *why, size_t why_cap) {
    if (!path || !path[0] || !path_clean(path)) {
        snprintf(why, why_cap, "bad path");
        return false;
    }
    /* relative paths are in the app's folder (in /spiffs without one) */
    int w = path[0] == '/' ? snprintf(out, cap, "%s", path)
                           : snprintf(out, cap, "%s/%s", s_box[0] ? s_box : "/spiffs", path);
    if (w < 0 || w >= (int)cap) {
        snprintf(why, why_cap, "path too long");
        return false;
    }
    if (!mini_app_sandboxed()) return true;
    size_t n = strlen(s_box);
    if (strncmp(out, s_box, n) != 0 || (out[n] != '/' && out[n] != '\0')) {
        snprintf(why, why_cap, "outside the app's folder %.40s", s_box);
        return false;
    }
    if (write && (out[n] == '\0' || strcmp(out + n, "/app.msbc") == 0 || strcmp(out + n, "/app.fasl") == 0)) {
        snprintf(why, why_cap, "the app's image is read-only");
        return false;
    }
    return true;
}

static bool box_privileged(void) { return !mini_app_sandboxed(); }

static const MiniAccessPolicy s_box_policy = { box_file, box_privileged };

void mini_app_sandbox_set(const char *dir) {
    snprintf(s_box, sizeof(s_box), "%s", dir ? dir : "");
    mini_set_access_policy(&s_box_policy);
}

bool mini_app_config_key_ok(const char *key) {
    if (!mini_app_sandboxed()) return true;
    static const char *const secret[] = { "password", "passwd", "secret", "token" };
    if (strncmp(key, "wifi_", 5) == 0 || strncmp(key, "channel_", 8) == 0 || strcmp(key, "boot_app") == 0) {
        return false;   /* the channel's key and settings are the system's */
    }
    for (size_t i = 0; i < sizeof(secret) / sizeof(secret[0]); i++) {
        if (strstr(key, secret[i])) return false;
    }
    return true;
}

Value mini_app_entry(MiniVM *vm, const char *path, const char *meta) {
    char id[32], name[64], desc[160], pages[8], version[16];
    mini_app_id(path, meta, id, sizeof(id));
    if (!meta || !mini_device_config_value(meta, "name", name, sizeof(name)) || !name[0]) {
        snprintf(name, sizeof(name), "%s", id);
    }
    if (!meta || !mini_device_config_value(meta, "description", desc, sizeof(desc))) desc[0] = '\0';
    if (!meta || !mini_device_config_value(meta, "pages", pages, sizeof(pages))) pages[0] = '\0';
    if (!meta || !mini_device_config_value(meta, "version", version, sizeof(version))) version[0] = '\0';
    Value list = mini_nil(), tmp = mini_nil();
    mini_push_root(vm, &list);
    mini_push_root(vm, &tmp);
    tmp = mini_make_string(vm, version);
    list = mini_make_cons(vm, tmp, list);
    list = mini_make_cons(vm, mini_make_fixnum(pages[0] ? (int64_t)strtoll(pages, NULL, 10) : 1), list);
    const char *fields[4] = { desc, name, id, path };
    for (int i = 0; i < 4; i++) {
        tmp = mini_make_string(vm, fields[i]);
        list = mini_make_cons(vm, tmp, list);
    }
    mini_pop_root(vm);
    mini_pop_root(vm);
    return list;
}

/* Appends s as JSON string contents; false when out is too small */
static bool json_append(char *out, size_t cap, size_t *n, const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        char esc[8];
        const char *add = esc;
        if (*p == '"' || *p == '\\') { esc[0] = '\\'; esc[1] = (char)*p; esc[2] = '\0'; }
        else if (*p == '\n') add = "\\n";
        else if (*p == '\r') add = "\\r";
        else if (*p == '\t') add = "\\t";
        else if (*p < 0x20) snprintf(esc, sizeof(esc), "\\u%04x", *p);
        else { esc[0] = (char)*p; esc[1] = '\0'; }
        size_t k = strlen(add);
        if (*n + k + 1 >= cap) return false;
        memcpy(out + *n, add, k);
        *n += k;
    }
    out[*n] = '\0';
    return true;
}

static bool json_raw(char *out, size_t cap, size_t *n, const char *s) {
    size_t k = strlen(s);
    if (*n + k + 1 >= cap) return false;
    memcpy(out + *n, s, k + 1);
    *n += k;
    return true;
}

size_t mini_app_json(char *out, size_t cap, const char *path, const char *meta, long size, int flags) {
    char id[32], name[64], desc[160], pages[8], version[16], ini[80], num[96], dir[64], key[48];
    mini_app_id(path, meta, id, sizeof(id));
    mini_app_dir_of(path, id, dir, sizeof(dir));
    mini_app_key(path, key, sizeof(key));
    if (!meta || !mini_device_config_value(meta, "name", name, sizeof(name)) || !name[0]) snprintf(name, sizeof(name), "%s", id);
    if (!meta || !mini_device_config_value(meta, "description", desc, sizeof(desc))) desc[0] = '\0';
    if (!meta || !mini_device_config_value(meta, "pages", pages, sizeof(pages)) || !pages[0]) snprintf(pages, sizeof(pages), "1");
    if (!meta || !mini_device_config_value(meta, "version", version, sizeof(version))) version[0] = '\0';
    snprintf(ini, sizeof(ini), "%s/config.ini", dir);
    size_t n = 0;
    bool ok = cap > 0;
    if (ok) out[0] = '\0';
    ok = ok && json_raw(out, cap, &n, "{\"key\":\"") && json_append(out, cap, &n, key);
    ok = ok && json_raw(out, cap, &n, "\",\"source\":\"") && json_append(out, cap, &n, mini_app_source(path));
    ok = ok && json_raw(out, cap, &n, "\",\"dir\":\"") && json_append(out, cap, &n, dir);
    ok = ok && json_raw(out, cap, &n, "\",\"path\":\"") && json_append(out, cap, &n, path);
    ok = ok && json_raw(out, cap, &n, "\",\"id\":\"") && json_append(out, cap, &n, id);
    ok = ok && json_raw(out, cap, &n, "\",\"name\":\"") && json_append(out, cap, &n, name);
    ok = ok && json_raw(out, cap, &n, "\",\"description\":\"") && json_append(out, cap, &n, desc);
    ok = ok && json_raw(out, cap, &n, "\",\"version\":\"") && json_append(out, cap, &n, version);
    ok = ok && json_raw(out, cap, &n, "\",\"ini\":\"") && json_append(out, cap, &n, ini);
    snprintf(num, sizeof(num), "\",\"pages\":%d,\"size\":%ld,\"boot\":%s,\"running\":%s,\"web\":%s",
             (int)strtoll(pages, NULL, 10), size, (flags & MINI_APP_BOOT) ? "true" : "false",
             (flags & MINI_APP_RUNNING) ? "true" : "false", (flags & MINI_APP_WEB) ? "true" : "false");
    ok = ok && json_raw(out, cap, &n, num);
    if (flags & MINI_APP_META) ok = ok && json_raw(out, cap, &n, ",\"meta\":\"") && json_append(out, cap, &n, meta ? meta : "") &&
                                  json_raw(out, cap, &n, "\"");
    ok = ok && json_raw(out, cap, &n, "}");
    return ok ? n : 0;
}

/* ------------------------------------------------------------------------- */
/* Button gestures                                                           */
/* ------------------------------------------------------------------------- */

static struct {
    bool down;              /* a press is in progress */
    bool up_pending;        /* released, waiting for the debounce time */
    bool long_sent, mid_sent, hold_sent;
    uint32_t down_at, up_at;
} s_btn;

/* The gesture times (config.ini [system] button_long_ms, button_mid_ms, button_hold_ms) */
static uint32_t s_btn_long_ms = MINI_BUTTON_LONG_MS;
static uint32_t s_btn_mid_ms = MINI_BUTTON_MID_MS;
static uint32_t s_btn_hold_ms = MINI_BUTTON_HOLD_MS;

static void call_optional(MiniVM *vm, const char *hook, const char *fallback) {
    Value fn = env_lookup_name(vm, hook);
    if (mini_is_closure(fn) || mini_is_cfunc(fn)) {
        mini_call(vm, fn, 0, NULL);
    } else if (fallback) {
        Value fb = env_lookup_name(vm, fallback);
        if (mini_is_closure(fb) || mini_is_cfunc(fb)) mini_call(vm, fb, 0, NULL);
    }
}

/* Calls hook when the app defines it; true when it did */
static bool call_if_defined(MiniVM *vm, const char *hook, int argc, Value *argv) {
    Value fn = env_lookup_name(vm, hook);
    if (!mini_is_closure(fn) && !mini_is_cfunc(fn)) return false;
    mini_call(vm, fn, argc, argv);
    return true;
}

/* The hardware pages (apps/lib/infopages.scm, in every app) are showing */
static bool info_pages_open(MiniVM *vm) {
    Value fn = env_lookup_name(vm, "info-active?");
    if (!mini_is_closure(fn) && !mini_is_cfunc(fn)) return false;
    return !mini_is_false(mini_call(vm, fn, 0, NULL));
}

/* ========================================================================= */
/* Boot progress bar                                                         */
/* ========================================================================= */
/* Shown while an app's Code ROM image is built at boot: no VM exists yet, so
 * it is drawn from C. A bar alone across the middle of a black screen: a
 * blue-gray track and a muted green fill, easy to overlook but clear on the
 * panel (it darkens dim colors); as it moves only its rows are drawn again. */
#define PROG_BG    0x0000  /* black */
#define PROG_TRACK 14890   /* #3a4654 blue-gray */
#define PROG_FILL  19533   /* #4f8a6c muted green */
#define PROG_BAR_H 6

static struct {
    bool shown;
    int permille;
    uint32_t at;
} s_prog;

static int prog_bar_y(void) { return (mini_gui_get_height() - PROG_BAR_H) / 2; }

static void prog_draw(int band_idx, int band_y, int band_h, void *userdata) {
    (void)band_idx; (void)band_y; (void)band_h; (void)userdata;
    int bar_w = mini_gui_get_width() - 80, y = prog_bar_y();
    int fill = bar_w * s_prog.permille / 1000;
    mini_gui_clear(PROG_BG);
    mini_gui_rect(40, y, bar_w, PROG_BAR_H, PROG_TRACK, true);
    if (fill > 0) mini_gui_rect(40, y, fill, PROG_BAR_H, PROG_FILL, true);
}

void mini_device_progress(int permille, uint32_t now_ms) {
    if (permille < 0) permille = 0;
    if (permille > 1000) permille = 1000;
    if (!s_prog.shown) {
        s_prog.shown = true;
        s_prog.permille = permille;
        s_prog.at = now_ms;
        mini_gui_render_bands(mini_gui_get_band_lines(), prog_draw, NULL);
        return;
    }
    /* a step of 2% at most every 80 ms, and always the end */
    if (permille == s_prog.permille) return;
    if (permille < 1000 && (permille - s_prog.permille < 20 || now_ms - s_prog.at < 80)) return;
    s_prog.permille = permille;
    s_prog.at = now_ms;
    int y = prog_bar_y();
    mini_gui_render_band_rows(y, y + PROG_BAR_H, mini_gui_get_band_lines(), prog_draw, NULL);
}

void mini_device_progress_end(void) {
    s_prog.shown = false;
}

int mini_device_progress_value(void) {
    return s_prog.shown ? s_prog.permille : -1;
}

/* ========================================================================= */
/* App failures: crash detection and the error screen                        */
/* ========================================================================= */
/* An app has crashed when its hooks keep failing: 10 errors within 10 s, 3
 * renders in a row that fail, or the heap running out. A single error (a
 * bad document, say) does not count. */
#define HEALTH_ERRORS    10
#define HEALTH_WINDOW_MS 10000
#define HEALTH_RENDERS   3

static struct {
    uint32_t at[HEALTH_ERRORS];   /* times of the latest errors, a ring */
    int n, head;
    int render_fails;
    char reason[160];
} s_health;

void mini_device_app_health_reset(void) {
    memset(&s_health, 0, sizeof(s_health));
}

bool mini_device_app_error(const char *where, const char *err, uint32_t now_ms) {
    snprintf(s_health.reason, sizeof(s_health.reason), "%s: %s", where ? where : "app", err ? err : "error");
    if (err && strstr(err, "out of memory")) return true;
    if (where && strcmp(where, "render") == 0) return ++s_health.render_fails >= HEALTH_RENDERS;
    s_health.at[s_health.head] = now_ms;
    s_health.head = (s_health.head + 1) % HEALTH_ERRORS;
    if (s_health.n < HEALTH_ERRORS) s_health.n++;
    /* the ring is full: its oldest entry (now at head) within the window? */
    return s_health.n == HEALTH_ERRORS && now_ms - s_health.at[s_health.head] <= HEALTH_WINDOW_MS;
}

void mini_device_app_render_ok(void) {
    s_health.render_fails = 0;
}

const char *mini_device_app_crash_reason(void) {
    return s_health.reason;
}

const char *mini_device_fail_title(const char *phase) {
    if (phase && strcmp(phase, "rom") == 0) return "\xe5\xba\x94\xe7\x94\xa8\xe9\x95\x9c\xe5\x83\x8f\xe6\x9e\x84\xe5\xbb\xba\xe5\xa4\xb1\xe8\xb4\xa5"; /* app image build failed */
    if (phase && strcmp(phase, "load") == 0) return "\xe5\xba\x94\xe7\x94\xa8\xe5\x8a\xa0\xe8\xbd\xbd\xe5\xa4\xb1\xe8\xb4\xa5"; /* app load failed */
    if (phase && strcmp(phase, "api") == 0) return "\xe5\xba\x94\xe7\x94\xa8\xe4\xb8\x8e\xe5\x9b\xba\xe4\xbb\xb6\xe4\xb8\x8d\xe5\x85\xbc\xe5\xae\xb9"; /* app incompatible with the firmware (API version) */
    return "\xe5\xba\x94\xe7\x94\xa8\xe8\xbf\x90\xe8\xa1\x8c\xe5\x87\xba\xe9\x94\x99"; /* app crashed */
}

/* Black screen: the title (muted amber), the app's name, the reason wrapped,
 * and that the settings come next; drawn from C, no VM needed */
#define ERR_FONT  7
#define ERR_TITLE 56619    /* #d9a45a */
#define ERR_NAME  59262    /* theme-text-main */
#define ERR_TEXT  48698    /* theme-text-sub */
#define ERR_HINT  42392    /* theme-text-muted */

static struct {
    int font;
    char title[48];
    char name[64];
    char reason[200];
} s_err;

static void err_center(int y, const char *s, uint16_t color) {
    int w = mini_font_text_width(s_err.font, s);
    mini_gui_text_styled((mini_gui_get_width() - w) / 2, y, s, color, s_err.font);
}

static void err_draw(int band_idx, int band_y, int band_h, void *userdata) {
    (void)band_idx; (void)band_y; (void)band_h; (void)userdata;
    int w = mini_gui_get_width(), cy = mini_gui_get_height() / 2;
    mini_gui_clear(0x0000);
    err_center(cy - 110, s_err.title, ERR_TITLE);
    err_center(cy - 80, s_err.name, ERR_NAME);
    mini_gui_text_wrap(24, cy - 40, w - 48, 150, s_err.reason, ERR_TEXT, s_err.font, 22);
    err_center(cy + 130, "\xe5\x8d\xb3\xe5\xb0\x86\xe8\xbf\x9b\xe5\x85\xa5\xe7\xb3\xbb\xe7\xbb\x9f\xe8\xae\xbe\xe7\xbd\xae", ERR_HINT); /* the settings come next */
}

void mini_device_error_screen(const char *title, const char *name, const char *reason) {
    static const char *const faces[] = { "simsun", "simhei", "msyh" };
    s_err.font = 0;
    for (size_t i = 0; i < sizeof(faces) / sizeof(faces[0]); i++) {
        if (mini_font_use(ERR_FONT, faces[i], 16)) {
            s_err.font = ERR_FONT;
            break;
        }
    }
    snprintf(s_err.title, sizeof(s_err.title), "%s", title ? title : "");
    snprintf(s_err.name, sizeof(s_err.name), "%s", name ? name : "");
    snprintf(s_err.reason, sizeof(s_err.reason), "%s", reason ? reason : "");
    mini_gui_render_bands(mini_gui_get_band_lines(), err_draw, NULL);
}

static bool s_system_gesture = true;

void mini_device_button_system_gesture(bool on) {
    s_system_gesture = on;
}

static uint32_t clamp_ms(uint32_t v, uint32_t lo, uint32_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

void mini_device_button_timing(uint32_t long_ms, uint32_t mid_ms, uint32_t hold_ms) {
    s_btn_long_ms = clamp_ms(long_ms ? long_ms : MINI_BUTTON_LONG_MS, 300, 3000);
    s_btn_mid_ms = clamp_ms(mid_ms ? mid_ms : MINI_BUTTON_MID_MS, s_btn_long_ms + 500, 15000);
    s_btn_hold_ms = clamp_ms(hold_ms ? hold_ms : MINI_BUTTON_HOLD_MS, s_btn_mid_ms + 1000, 30000);
}

void mini_device_button_get_timing(uint32_t *long_ms, uint32_t *mid_ms, uint32_t *hold_ms) {
    if (long_ms) *long_ms = s_btn_long_ms;
    if (mid_ms) *mid_ms = s_btn_mid_ms;
    if (hold_ms) *hold_ms = s_btn_hold_ms;
}

static uint32_t config_ms(const char *text, const char *key) {
    char v[16];
    if (!text || !mini_device_config_value(text, key, v, sizeof(v)) || !v[0]) return 0;
    long long ms = strtoll(v, NULL, 10);
    return ms > 0 ? (uint32_t)ms : 0;
}

void mini_device_button_config(const char *config_text) {
    mini_device_button_timing(config_ms(config_text, "button_long_ms"), config_ms(config_text, "button_mid_ms"),
                              config_ms(config_text, "button_hold_ms"));
}

/* How far a bar from start to the time it arms has filled, in permille */
static int hold_permille(uint32_t held, uint32_t start, uint32_t armed) {
    return held <= start ? 0 : (int)((held - start) * 1000 / (armed - start));
}

/* The corner bars while KEY 18 is held (mini_gui_set_hold_overlay) */
static void button_overlay(uint32_t held) {
    uint32_t bar_start = s_btn_long_ms * 5 / 8;
    if (held < bar_start) {
        mini_gui_set_hold_overlay(0, 0, 0, 0);
        return;
    }
    int flags = MINI_HOLD_VISIBLE;
    int long_pm = hold_permille(held, bar_start, s_btn_long_ms);
    if (s_btn.long_sent) flags |= MINI_HOLD_LONG_ARMED;
    int mid_pm = 0, hold_pm = 0;
    if (held >= s_btn_long_ms) {
        flags |= MINI_HOLD_MID_VISIBLE;
        mid_pm = hold_permille(held, s_btn_long_ms, s_btn_mid_ms);
        if (s_btn.mid_sent) flags |= MINI_HOLD_MID_ARMED;
    }
    if (s_system_gesture && held >= s_btn_mid_ms) {
        flags |= MINI_HOLD_INFO_VISIBLE;
        hold_pm = hold_permille(held, s_btn_mid_ms, s_btn_hold_ms);
        if (s_btn.hold_sent) flags |= MINI_HOLD_INFO_ARMED;
    }
    mini_gui_set_hold_overlay(flags, long_pm, mid_pm, hold_pm);
}

MiniButtonEvent mini_device_button_update(MiniVM *vm, bool pressed, uint32_t now_ms) {
    if (pressed) {
        s_btn.up_pending = false;
        if (!s_btn.down) {
            s_btn.down = true;
            s_btn.long_sent = s_btn.mid_sent = s_btn.hold_sent = false;
            s_btn.down_at = now_ms;
        }
        uint32_t held = now_ms - s_btn.down_at;
        /* thresholds only arm the gestures; they run on release */
        if (held >= s_btn_long_ms) s_btn.long_sent = true;
        if (held >= s_btn_mid_ms) s_btn.mid_sent = true;
        if (s_system_gesture && held >= s_btn_hold_ms) s_btn.hold_sent = true;
        button_overlay(held);
        return MINI_BUTTON_NONE;
    }
    if (!s_btn.down) return MINI_BUTTON_NONE;
    if (!s_btn.up_pending) {
        s_btn.up_pending = true;
        s_btn.up_at = now_ms;
    }
    if (now_ms - s_btn.up_at < MINI_BUTTON_DEBOUNCE_MS) return MINI_BUTTON_NONE;
    s_btn.down = s_btn.up_pending = false;
    mini_gui_set_hold_overlay(0, 0, 0, 0);
    /* the hold: the hardware pages open (or close); while they show, they take
     * the presses: short turns them, long closes them, mid opens the settings */
    if (s_btn.hold_sent) {
        if (vm) call_if_defined(vm, "info-long-press", 0, NULL);
        return MINI_BUTTON_INFO;
    }
    if (s_btn.mid_sent) {
        if (vm && info_pages_open(vm)) return MINI_BUTTON_SYSTEM;  /* the host opens the settings */
        if (vm) call_optional(vm, "app-mid-press", NULL);
        return MINI_BUTTON_MID;
    }
    if (s_btn.long_sent) {
        if (vm && info_pages_open(vm)) call_if_defined(vm, "info-long-press", 0, NULL);
        else if (vm) call_optional(vm, "app-long-press", NULL);
        return MINI_BUTTON_LONG;
    }
    if (vm && info_pages_open(vm)) call_if_defined(vm, "info-button", 0, NULL);
    else if (vm) call_optional(vm, "app-button", NULL);
    return MINI_BUTTON_SHORT;
}

uint32_t mini_device_button_held_ms(uint32_t now_ms) {
    return s_btn.down && !s_btn.up_pending ? now_ms - s_btn.down_at : 0;
}

/* Returns the string cached in slot when its text is unchanged, so fields
 * that change once a minute or a day allocate nothing on most ticks.
 * Strings are immutable in this VM, so sharing them is safe. */
static Value cached_string(MiniVM *vm, Value *cache, int slot, const char *text) {
    Value old = mini_vector_ref(vm, *cache, (size_t)slot);
    if (mini_is_string(old) && strcmp(mini_string_data(old), text) == 0) return old;
    Value s = mini_make_string(vm, text); /* *cache is rooted by the caller */
    mini_vector_set(vm, *cache, (size_t)slot, s);
    return s;
}

#define CLOCK_CACHE_GLOBAL "%clock-strings"
#define CLOCK_CACHE_SLOTS  5

/* The wall time of the last clock tick: (time-local) */
static MiniWallTime s_last_wall;

void mini_device_clock_tick(MiniVM *vm, const MiniWallTime *t, int tick, int lux, int backlight) {
    static const char *const weekdays[] = { "周日", "周一", "周二", "周三", "周四", "周五", "周六" };
    char hm[16], sec[8], week[16], date[32], lunar[48];
    if (!vm || !t) return;
    s_last_wall = *t;
    mini_backlight_auto_step();
    backlight = mini_backlight_get();

    if (t->synced) {
        snprintf(hm, sizeof(hm), "%02d:%02d", t->hour, t->minute);
        snprintf(sec, sizeof(sec), ":%02d", t->second);
        snprintf(week, sizeof(week), "%s", weekdays[((t->wday % 7) + 7) % 7]);
        snprintf(date, sizeof(date), "%d月%d日", t->month, t->day);
        if (!mini_lunar_format(t->year, t->month, t->day, lunar, sizeof(lunar))) lunar[0] = '\0';
    } else {
        snprintf(hm, sizeof(hm), "%s", MINI_CLOCK_PLACEHOLDER_HM);
        snprintf(sec, sizeof(sec), "%s", MINI_CLOCK_PLACEHOLDER_SEC);
        snprintf(week, sizeof(week), "%s", MINI_CLOCK_PLACEHOLDER_WEEK);
        snprintf(date, sizeof(date), "%s", MINI_CLOCK_PLACEHOLDER_DATE);
        lunar[0] = '\0';
    }

    /* Root every string: each allocation may trigger a moving GC */
    Value cache = env_lookup_name(vm, CLOCK_CACHE_GLOBAL);
    if (!mini_is_vector(cache) || mini_vector_length(cache) != CLOCK_CACHE_SLOTS) {
        cache = mini_make_vector(vm, CLOCK_CACHE_SLOTS, mini_false());
        mini_push_root(vm, &cache);
        mini_set_global(vm, CLOCK_CACHE_GLOBAL, cache);
        mini_pop_root(vm);
    }
    Value args[8];
    const char *strs[4] = { hm, sec, week, date };
    for (int i = 0; i < 8; i++) args[i] = mini_void();
    mini_push_root(vm, &cache);
    for (int i = 0; i < 8; i++) mini_push_root(vm, &args[i]);
    for (int i = 0; i < 4; i++) args[i] = cached_string(vm, &cache, i, strs[i]);
    args[4] = mini_make_fixnum(lux);
    args[5] = mini_make_fixnum(backlight);
    args[6] = mini_make_fixnum(tick);
    args[7] = cached_string(vm, &cache, 4, lunar);
    Value call_args[8];
    memcpy(call_args, args, sizeof(call_args));
    mini_device_call_hook(vm, "app-tick", NULL, 8, call_args);
    memcpy(call_args, args, sizeof(call_args));
    call_if_defined(vm, "info-tick", 8, call_args);  /* the hardware pages' clock and figures */
    for (int i = 0; i < 8; i++) mini_pop_root(vm);
    mini_pop_root(vm);
}

static bool lunar_args(int argc, Value *argv, int *y, int *m, int *d) {
    if (argc != 3 || !mini_is_fixnum(argv[0]) || !mini_is_fixnum(argv[1]) || !mini_is_fixnum(argv[2])) return false;
    *y = (int)mini_to_fixnum(argv[0]);
    *m = (int)mini_to_fixnum(argv[1]);
    *d = (int)mini_to_fixnum(argv[2]);
    return true;
}

/* (lunar-date y m d) => (month day leap?) or #f when out of range */
static Value prim_lunar_date(MiniVM *vm, int argc, Value *argv) {
    int y, m, d, lm = 0, ld = 0;
    bool leap = false;
    if (!lunar_args(argc, argv, &y, &m, &d) || !mini_lunar_from_solar(y, m, d, &lm, &ld, &leap)) return mini_false();
    Value res = mini_make_cons(vm, mini_make_bool(leap), mini_nil());
    mini_push_root(vm, &res);
    res = mini_make_cons(vm, mini_make_fixnum(ld), res);
    res = mini_make_cons(vm, mini_make_fixnum(lm), res);
    mini_pop_root(vm);
    return res;
}

/* (lunar-date-string y m d) => display text for the LCD, or #f */
static Value prim_lunar_date_string(MiniVM *vm, int argc, Value *argv) {
    int y, m, d;
    char buf[48];
    if (!lunar_args(argc, argv, &y, &m, &d) || !mini_lunar_format(y, m, d, buf, sizeof(buf))) return mini_false();
    return mini_make_string(vm, buf);
}

/* ------------------------------------------------------------------------- */
/* (system-info): one alist with everything the platform can report.         */
/* Built from whatever primitives the platform registered, so the same call  */
/* works on the firmware, the WASM simulator and PC builds.                  */
/* ------------------------------------------------------------------------- */

/* Calls a zero-argument global if bound; #f otherwise */
static Value call_global0(MiniVM *vm, const char *name) {
    Value fn = env_lookup_name(vm, name);
    if (!mini_is_closure(fn) && !mini_is_cfunc(fn)) return mini_false();
    Value r = mini_call(vm, fn, 0, NULL);
    if (mini_get_error(vm)) {
        mini_clear_error(vm);
        return mini_false();
    }
    return r;
}

/* Prepends (key . val) to *alist; *alist must already be a registered root */
static void alist_push(MiniVM *vm, Value *alist, const char *key, Value val) {
    mini_push_root(vm, &val);
    Value k = mini_make_symbol(vm, key);
    mini_push_root(vm, &k);
    Value pair = mini_make_cons(vm, k, val);
    mini_push_root(vm, &pair);
    *alist = mini_make_cons(vm, pair, *alist);
    mini_pop_root(vm);
    mini_pop_root(vm);
    mini_pop_root(vm);
}

static Value list_reverse(MiniVM *vm, Value l) {
    Value res = mini_nil();
    mini_push_root(vm, &l);
    mini_push_root(vm, &res);
    while (mini_is_pair(l)) {
        res = mini_make_cons(vm, mini_car(l), res);
        l = mini_cdr(l);
    }
    mini_pop_root(vm);
    mini_pop_root(vm);
    return res;
}

typedef struct {
    const char *key;
    const char *global; /* zero-argument primitive to call */
} InfoField;

/* Appends (section . ((key . value) ...)) built from the given fields */
static void add_section(MiniVM *vm, Value *root, const char *section, const InfoField *fields, size_t n) {
    Value sec = mini_nil();
    mini_push_root(vm, &sec);
    for (size_t i = 0; i < n; i++) {
        Value v = call_global0(vm, fields[i].global);
        alist_push(vm, &sec, fields[i].key, v);
    }
    sec = list_reverse(vm, sec);
    alist_push(vm, root, section, sec);
    mini_pop_root(vm);
}

/* (time-local): (year month day hour minute second weekday) of the last
 * clock tick (the app-tick wall time), #f before the clock is synced */
static Value prim_time_local(MiniVM *vm, int argc, Value *argv) {
    (void)argc; (void)argv;
    if (!s_last_wall.synced) return mini_false();
    const int f[7] = { s_last_wall.year, s_last_wall.month, s_last_wall.day, s_last_wall.hour,
                       s_last_wall.minute, s_last_wall.second, ((s_last_wall.wday % 7) + 7) % 7 };
    Value list = mini_nil();
    mini_push_root(vm, &list);
    for (int i = 6; i >= 0; i--) list = mini_make_cons(vm, mini_make_fixnum(f[i]), list);
    mini_pop_root(vm);
    return list;
}

static Value prim_system_info(MiniVM *vm, int argc, Value *argv) {
    (void)argc; (void)argv;
    static const InfoField memory[] = {
        { "free-heap", "free-heap" }, { "largest-block", "max-alloc-heap" },
        { "min-free", "min-free-heap" }, { "total", "heap-total" },
        { "vm-used", "vm-allocated-bytes" }, { "vm-heap", "vm-heap-size" },
        { "app-rom", "vm-rom-bytes" },
        { "stack-depth", "vm-stack-depth" }, { "frame-depth", "vm-frame-depth" },
    };
    static const InfoField flash[] = {
        { "total", "flash-total-bytes" }, { "app-image", "flash-app-bytes" },
        { "app-partition", "flash-app-partition-bytes" },
        { "app-rom", "vm-rom-bytes" }, { "app-rom-partition", "app-rom-capacity" },
    };
    static const InfoField network[] = {
        { "connected", "wifi-connected?" }, { "ssid", "wifi-ssid" },
        { "ip", "wifi-ip" }, { "rssi", "wifi-rssi" },
    };
    static const InfoField time_fields[] = {
        { "ntp-synced", "time-synced?" }, { "uptime-ms", "millis" },
    };
    static const InfoField display[] = {
        { "model", "display-model" }, { "width", "gui-width" }, { "height", "gui-height" },
        { "mode", "gui-mode" }, { "band-lines", "gui-band-lines" },
        { "color-bits", "gui-color-bits" }, { "color-scheme", "gui-color-scheme" },
        { "color", "gui-color-format" }, { "buffer-bytes", "gui-buffer-ram" },
        { "dma-buffer-bytes", "lcd-dma-ram" },
    };
    static const InfoField peripherals[] = {
        { "backlight", "backlight-get" }, { "light-adc", "light-read" },
        { "light-lux", "light-lux" }, { "ws2812", "ws2812-get" },
        { "button", "button-pressed?" },
    };
    Value root = mini_nil();
    mini_push_root(vm, &root);
    alist_push(vm, &root, "chip", call_global0(vm, "chip-info"));
    add_section(vm, &root, "memory", memory, sizeof(memory) / sizeof(memory[0]));
    add_section(vm, &root, "flash", flash, sizeof(flash) / sizeof(flash[0]));
    add_section(vm, &root, "network", network, sizeof(network) / sizeof(network[0]));
    add_section(vm, &root, "time", time_fields, sizeof(time_fields) / sizeof(time_fields[0]));
    add_section(vm, &root, "display", display, sizeof(display) / sizeof(display[0]));
    add_section(vm, &root, "peripherals", peripherals, sizeof(peripherals) / sizeof(peripherals[0]));
    root = list_reverse(vm, root);
    mini_pop_root(vm);
    return root;
}

void mini_register_device_lib(MiniVM *vm) {
    if (!vm) return;
    static const MiniPrimEntry funcs[] = {
        { "lunar-date",        prim_lunar_date },
        { "lunar-date-string", prim_lunar_date_string },
        { "system-info",       prim_system_info },
        { "time-local",        prim_time_local },
    };
    mini_register_prim_table(vm, funcs, sizeof(funcs) / sizeof(funcs[0]));
}

/* ========================================================================= */
/* config.ini (firmware SPIFFS and the WASM device's simulated SPIFFS)       */
/* ========================================================================= */

const char mini_device_config_template[] =
    "; Device configuration (read at boot)\n"
    "; ';' or '#' starts a comment; unknown keys are ignored.\n"
    "config_version = 3\n"
    "\n"
    "[system]\n"
    "boot_app =             ; app started at boot: its image under /spiffs, apps/<id>/app.msbc (market)\n"
    "                       ; or user/<id>/app.msbc; empty: app.msbc. Settings: KEY 18 held 8 s\n"
    "                       ; (hardware pages), then a 3.5 s press there\n"
    "device_name = \xe6\xa1\x8c\xe9\x9d\xa2\xe5\xb1\x8f ; the web page's title (the settings hotspot is Desk-XXXX)\n"
    "app_storage_kb = 256   ; storage of each app's folder, its image included (64..1024)\n"
    "button_long_ms = 800   ; KEY 18: a press this long is the app's long press (300..3000)\n"
    "button_mid_ms = 3500   ; the app's mid press; on the hardware pages: system settings\n"
    "button_hold_ms = 8000  ; the hold that opens the hardware pages (up to 30000)\n"
    "web_in_app = 1         ; 1: the web page and HTTP API also while an app runs; 0: only in the\n"
    "                       ; system settings (the app's VM gets the HTTP server's RAM)\n"
    "factory_reset_key = 1  ; 1: KEY 18 held from power-on for 10 s erases the app ROM and opens the\n"
    "                       ; built-in settings (resets: settings menu > factory reset); 0: off\n"
    "\n"
    "[wifi]\n"
    "wifi_ssid =            ; network to join (set on the settings web page)\n"
    "wifi_password =\n"
    "\n"
    "[display]\n"
    "mode = band            ; band (a few rows re-rendered per slice), full (whole frame) or\n"
    "                       ; direct (no buffer: every drawn span goes straight to the panel, slow)\n"
    "band_lines = 13        ; rows per band; band RAM = width x rows x color_bits / 8 (13 -> 8.3 KB)\n"
    "color_bits = 16        ; bits per stored pixel: 16 (RGB565), 8, 4, 2 or 1 (palette indices)\n"
    "                       ; full frame 320x480: 300 / 150 / 75 / 37.5 / 18.8 KB\n"
    "palette = color        ; color scheme: color, gray, eink, green, amber, cyan, gameboy, invert\n"
    "auto_backlight = 1     ; 1: brightness follows the light sensor; 0: the fixed backlight below\n"
    "backlight = 220        ; 0..255 while auto_backlight = 0 ((backlight-set n) sets both)\n"
    "panel = ips            ; screen type: ips (colors inverted in the driver) or tft (not inverted);\n"
    "                       ; colors like a photo negative: the other type (settings page too)\n"
    "lcd_gamma = driver     ; panel gamma (E0/E1), read at boot: driver (the driver's tables, default),\n"
    "                       ; chip (the controller's own curve after reset) or custom: the driver's\n"
    "                       ; tables with lcd_gamma_pos / _neg written over them from the first byte\n"
    "lcd_gamma_pos =        ; custom positive gamma, hex bytes; bytes left out keep the driver's. Driver:\n"
    "                       ; ILI9488 (15) 00 03 09 08 16 0A 3F 78 4C 09 0A 08 16 1A 0F\n"
    "                       ; ST7796 (14)  F0 09 0B 06 04 15 2F 54 42 3C 17 14 18 1B\n"
    "lcd_gamma_neg =        ; custom negative gamma, the same way. Driver:\n"
    "                       ; ILI9488 (15) 00 16 19 03 0F 05 32 45 46 04 0E 0D 35 37 0F\n"
    "                       ; ST7796 (14)  E0 09 0B 06 04 03 2B 43 42 3B 16 14 17 1B\n"
    "lcd_vcom =             ; VCOM (C5) bytes instead of the driver's (ILI9488: 00 12 80, ST7796: 18)\n"
    "color_format = rgb565  ; panel bus format: rgb565 (2 B/pixel) or rgb666 (3 B/pixel)\n"
    "width = 320\n"
    "height = 480\n"
    "lcd_dma = 1            ; 1 = I2S DMA bus, 0 = GPIO bit-bang (no DMA buffer); direct mode always bit-bangs\n"
    "lcd_dma_lines = 0      ; rows per DMA transfer, 0 = 13 (or the band, if shorter)\n"
    "                       ; DMA RAM = 2 x rows x width x (2 or 3) B; rgb666 adds 1x for staging\n"
    "lcd_clock_hz = 20000000 ; WR clock to try first; verified at boot, lowered until writes land\n"
    "\n"
    "[memory]\n"
    "reserved_heap = 65536  ; heap kept free for WiFi/lwIP/HTTP at run time (bytes), measured once WiFi joined\n"
    "max_heap = 131072      ; VM heap limit (bytes); the VM gets free heap - reserved_heap\n"
    "vm_eden = 4096         ; VM nursery (bytes)\n"
    "vm_stack = 1024        ; VM value stack (slots, 4 B each)\n"
    "vm_frames = 256        ; VM call frames\n"
    "app_xip = 1            ; 1 = run the app from Flash (approm), 0 = load it to RAM\n"
    "http_stack = 6144      ; HTTP server task stack (bytes)\n"
    "\n"
    "[debug]\n"
    "stack_trace = 0        ; 1: capture stack trace on errors (~2.5 KB RAM); 0: disabled (default)\n"
    "\n"
    "[text]\n"
    "; one-line text (titles, values, names) wider than its box:\n"
    "marquee = pixel        ; none (cut with an ellipsis), pixel (moves by pixels), char (by whole\n"
    "                       ; characters) or page (the box-wide pieces in turn)\n"
    "marquee_fps = 20       ; refreshes per second, 0.1..25\n"
    "marquee_step = 1       ; pixels (pixel) or characters (char) per refresh\n"
    "marquee_hold = 1       ; seconds the start stays after a jump back, e.g. 0.5 (0: none)\n"
    "; wrapped text (headlines, descriptions) taller than its box:\n"
    "wrap_marquee = line    ; none (cut with an ellipsis), pixel (rolls up by pixels), line (up by\n"
    "                       ; whole lines) or page (the next box-full of lines each time)\n"
    "wrap_marquee_fps = 0.5 ; refreshes per second, 0.1..25\n"
    "wrap_marquee_step = 1  ; pixels (pixel) or lines (line) per refresh\n"
    "wrap_marquee_hold = 1  ; seconds the first line stays after a jump back (0: none)\n"
    "\n"
    "[fonts]\n"
    "font_order = app, fontrom, spiffs ; sources searched glyph by glyph, in this order:\n"
    ";   app      the font the app carries (embedded at build time)\n"
    ";   fontrom  system font ROM (flash partition), font named <face>-<size>\n"
    ";   spiffs   /spiffs/fonts/<face>-<size>.mfnt\n"
    "; e.g. 'fontrom, app': a font ROM font of the same name comes before the app's own\n"
    "font_override =        ; replace fonts by name: name:replacement, comma separated. Empty:\n"
    ";                        system_16:simsun-16 (the font ROM's full GB2312 Song 16 px, shipped\n"
    ";                        with the firmware; the built-in font when it is absent); none: no\n"
    ";                        replacement. Examples:\n"
    ";   system_16:simsun-16  built-in 16 px font (0) drawn with simsun-16\n"
    ";   clock_32:digit-32    built-in clock font (1)\n"
    ";   simhei-24:kaiti-24   an app's simhei-24 replaced by kaiti-24\n"
    "font_server = http://124.222.231.186:8090 ; font server (minischeme-proxy font_server.py, no\n"
    ";                        password): glyphs the board's fonts lack (traditional and rare\n"
    ";                        characters, Korean, emoji) are fetched there and kept in /spiffs/fonts\n"
    "font_server_on = 1     ; 0: never ask the font server\n"
    "font_server_first = 0  ; 0: the board's fonts first, the server fills gaps; 1: the server's face\n"
    ";                        first for system-font text (any face it has; the board's glyphs stand\n"
    ";                        in until the server's arrive)\n"
    "font_server_face =     ; the face asked for, e.g. msyh (empty: the system font's, simsun)\n"
    "\n"
    "[app]\n"
    "; keys every app reads with (config-get \"key\"); an app's own config.ini in its folder\n"
    "; (made from the app's defaults on its first start) overrides them\n"
    "proxy =                ; the app proxy / protocol converter (tools/app_proxy.py), http://<host>:8087\n"
    "proxy_password =       ; app service password for that server (plain text): the device registers\n"
    "                       ; with it; bound to the server once registered (change the address first)\n"
    "https_proxy =          ; protocol converter for apps' https:// data (<it>/fetch?url=...); empty: proxy\n"
    "https_proxy_password = ; app service password for https_proxy (empty: the one of the same server)\n"
    "; The device registers with a converter by itself on first use (its MAC as its id, a hash of the\n"
    "; server's app service password; the password is never sent) and keeps the key it gets in the same\n"
    "; value: proxy = <key>@http://<host>:8087. Each server has its own key and password; an app's own\n"
    "; proxy_url / converter_url (its config.ini) keep theirs there, with proxy_url_password /\n"
    "; converter_url_password. No password ([server] hardware_password neither): plain HTTP.\n"
    "carousel = 1           ; 1: auto-turn pages in multi-page apps; 0: manual only\n"
    "carousel_seconds = 10  ; seconds per page (or comma-separated seconds per page)\n"
    "\n"
    "[market]\n"
    "; the app market: install and update apps in the system settings (the hardware pages' mid press, or\n"
    "; the web page's \"enter settings mode\"); while an app runs the web page lists what is installed and new\n"
    "market_url =           ; index.json and app images; empty: <proxy>/market (tools/app_proxy.py)\n"
    "market_auto_update = 0 ; 1: an app's boot checks once when the network comes up; with updates the\n"
    ";                        settings install them and return to the app (no network: skipped, no retry)\n";

/* Appended to a config.ini written before boot_app / WiFi / proxy existed */
const char mini_device_config_system_section[] =
    "\n"
    "[system]\n"
    "boot_app =             ; app started at boot: its image under /spiffs, apps/<id>/app.msbc (market)\n"
    "                       ; or user/<id>/app.msbc; empty: app.msbc. Settings: KEY 18 held 8 s\n"
    "                       ; (hardware pages), then a 3.5 s press there\n"
    "device_name = \xe6\xa1\x8c\xe9\x9d\xa2\xe5\xb1\x8f ; the web page's title (the settings hotspot is Desk-XXXX)\n"
    "app_storage_kb = 256   ; storage of each app's folder, its image included (64..1024)\n"
    "button_long_ms = 800   ; KEY 18: a press this long is the app's long press (300..3000)\n"
    "button_mid_ms = 3500   ; the app's mid press; on the hardware pages: system settings\n"
    "button_hold_ms = 8000  ; the hold that opens the hardware pages (up to 30000)\n"
    "web_in_app = 1         ; 1: the web page and HTTP API also while an app runs; 0: only in the\n"
    "                       ; system settings (the app's VM gets the HTTP server's RAM)\n"
    "factory_reset_key = 1  ; 1: KEY 18 held from power-on for 10 s erases the app ROM and opens the\n"
    "                       ; built-in settings (resets: settings menu > factory reset); 0: off\n"
    "\n"
    "[wifi]\n"
    "wifi_ssid =            ; network to join (set on the settings web page)\n"
    "wifi_password =\n"
    "\n"
    "[app]\n"
    "proxy =                ; tools/app_proxy.py on a PC, http://<pc-ip>:8087: HTTPS sources for the apps\n";

/* Appended to a config.ini written before the [fonts] options existed */
const char mini_device_config_fonts_section[] =
    "\n"
    "[fonts]\n"
    "font_order = app, fontrom, spiffs ; sources searched glyph by glyph, in this order:\n"
    ";   app      the font the app carries (embedded at build time)\n"
    ";   fontrom  system font ROM (flash partition), font named <face>-<size>\n"
    ";   spiffs   /spiffs/fonts/<face>-<size>.mfnt\n"
    "; e.g. 'fontrom, app': a font ROM font of the same name comes before the app's own\n"
    "font_override =        ; replace fonts by name: name:replacement, comma separated. Empty:\n"
    ";                        system_16:simsun-16 (the font ROM's full GB2312 Song 16 px, shipped\n"
    ";                        with the firmware; the built-in font when it is absent); none: no\n"
    ";                        replacement. Examples:\n"
    ";   system_16:simsun-16  built-in 16 px font (0) drawn with simsun-16\n"
    ";   clock_32:digit-32    built-in clock font (1)\n"
    ";   simhei-24:kaiti-24   an app's simhei-24 replaced by kaiti-24\n"
    "font_server = http://124.222.231.186:8090 ; font server (minischeme-proxy font_server.py, no\n"
    ";                        password): glyphs the board's fonts lack (traditional and rare\n"
    ";                        characters, Korean, emoji) are fetched there and kept in /spiffs/fonts\n"
    "font_server_on = 1     ; 0: never ask the font server\n"
    "font_server_first = 0  ; 0: the board's fonts first, the server fills gaps; 1: the server's face\n"
    ";                        first for system-font text (any face it has; the board's glyphs stand\n"
    ";                        in until the server's arrive)\n"
    "font_server_face =     ; the face asked for, e.g. msyh (empty: the system font's, simsun)\n";

/* Appended to a config.ini written before the [text] options existed */
const char mini_device_config_text_section[] =
    "\n"
    "[text]\n"
    "; one-line text (titles, values, names) wider than its box:\n"
    "marquee = pixel        ; none (cut with an ellipsis), pixel (moves by pixels), char (by whole\n"
    "                       ; characters) or page (the box-wide pieces in turn)\n"
    "marquee_fps = 20       ; refreshes per second, 0.1..25\n"
    "marquee_step = 1       ; pixels (pixel) or characters (char) per refresh\n"
    "marquee_hold = 1       ; seconds the start stays after a jump back, e.g. 0.5 (0: none)\n"
    "; wrapped text (headlines, descriptions) taller than its box:\n"
    "wrap_marquee = line    ; none (cut with an ellipsis), pixel (rolls up by pixels), line (up by\n"
    "                       ; whole lines) or page (the next box-full of lines each time)\n"
    "wrap_marquee_fps = 0.5 ; refreshes per second, 0.1..25\n"
    "wrap_marquee_step = 1  ; pixels (pixel) or lines (line) per refresh\n"
    "wrap_marquee_hold = 1  ; seconds the first line stays after a jump back (0: none)\n";

/* Decimal digits at the start of s (the WASM libc has no atoi) */
static int parse_uint(const char *s) {
    int n = 0;
    while (*s == ' ') s++;
    while (*s >= '0' && *s <= '9' && n < 100000) n = n * 10 + (*s++ - '0');
    return n;
}

/* "12", "0.5", "2.5": tenths */
static int parse_tenths(const char *s) {
    int n = parse_uint(s) * 10;
    const char *dot = strchr(s, '.');
    if (dot && dot[1] >= '0' && dot[1] <= '9') n += dot[1] - '0';
    return n;
}

/* "1", "0.5", "0.25": milliseconds */
static int parse_ms(const char *s) {
    int ms = parse_uint(s) * 1000;
    const char *dot = strchr(s, '.');
    if (dot) {
        int scale = 100;
        for (const char *d = dot + 1; *d >= '0' && *d <= '9' && scale > 0; d++, scale /= 10) ms += (*d - '0') * scale;
    }
    return ms;
}

/* One kind of text: keys <prefix>marquee, _fps, _step and _hold */
static void apply_marquee(const char *ini, int which, const char *prefix, int mode, int fps_x10, int step) {
    char key[24], v[16];
    snprintf(key, sizeof(key), "%smarquee", prefix);
    if (ini && mini_device_config_value(ini, key, v, sizeof(v))) {
        int m = mini_gui_marquee_parse(which, v);
        if (m >= 0) mode = m;
    }
    snprintf(key, sizeof(key), "%smarquee_fps", prefix);
    if (ini && mini_device_config_value(ini, key, v, sizeof(v))) fps_x10 = parse_tenths(v);
    snprintf(key, sizeof(key), "%smarquee_step", prefix);
    if (ini && mini_device_config_value(ini, key, v, sizeof(v))) step = parse_uint(v);
    int hold_ms = 1000;
    snprintf(key, sizeof(key), "%smarquee_hold", prefix);
    if (ini && mini_device_config_value(ini, key, v, sizeof(v))) hold_ms = parse_ms(v);
    mini_gui_set_marquee_hold(which, hold_ms);
    mini_gui_set_marquee(which, mode, fps_x10, step);
}

void mini_device_apply_text_config(const char *ini) {
    apply_marquee(ini, MINI_GUI_TEXT_LINE, "", MINI_GUI_MARQUEE_PIXEL, 200, 1);
    apply_marquee(ini, MINI_GUI_TEXT_WRAP, "wrap_", MINI_GUI_MARQUEE_LINE, 5, 1);
}

/* With font_override empty or missing the system 16 px font is drawn with
 * simsun-16 (full GB2312) when the font ROM has it: the firmware's font ROM
 * image carries it, and the built-in font keeps only ~700 characters. A
 * missing simsun-16 leaves the built-in font in place. "none" turns it off. */
#define DEFAULT_FONT_OVERRIDE "system_16:simsun-16"

const char *mini_device_apply_font_config(const char *ini) {
    char order[160], overrides[400];
    bool has_order = ini && mini_device_config_value(ini, "font_order", order, sizeof(order));
    bool has_over = ini && mini_device_config_value(ini, "font_override", overrides, sizeof(overrides));
    if (!has_over || overrides[0] == '\0') {
        snprintf(overrides, sizeof(overrides), "%s", DEFAULT_FONT_OVERRIDE);
    } else if (strcmp(overrides, "none") == 0) {
        overrides[0] = '\0';
    }
    const char *why = mini_font_configure(has_order ? order : NULL, overrides);
    mini_device_apply_fontnet_config(ini);
    return why;
}

void mini_device_apply_fontnet_config(const char *ini) {
    /* the font server: on by default, at the project's server unless set */
    char url[128], on[8], first[8], face[24];
    bool has_url = ini && mini_device_config_value(ini, "font_server", url, sizeof(url));
    bool has_on = ini && mini_device_config_value(ini, "font_server_on", on, sizeof(on));
    bool has_first = ini && mini_device_config_value(ini, "font_server_first", first, sizeof(first));
    bool has_face = ini && mini_device_config_value(ini, "font_server_face", face, sizeof(face));
    mini_fontnet_configure(has_url ? url : NULL, !has_on || strcmp(on, "0") != 0,
                           has_first && strcmp(first, "1") == 0, has_face ? face : "");
}

static bool key_equals(const char *p, size_t n, const char *key) {
    if (strlen(key) != n) return false;
    for (size_t i = 0; i < n; i++) {
        if (tolower((unsigned char)p[i]) != tolower((unsigned char)key[i])) return false;
    }
    return true;
}

bool mini_device_config_value(const char *text, const char *key, char *out, size_t cap) {
    if (!text || !key || !out || cap == 0) return false;
    for (const char *line = text; *line; ) {
        const char *end = strchr(line, '\n');
        if (!end) end = line + strlen(line);
        const char *stop = line;
        while (stop < end && *stop != ';' && *stop != '#') stop++;
        const char *eq = line;
        while (eq < stop && *eq != '=') eq++;
        if (eq < stop) {
            const char *k = line, *k_end = eq;
            while (k < k_end && (*k == ' ' || *k == '\t')) k++;
            while (k_end > k && (k_end[-1] == ' ' || k_end[-1] == '\t')) k_end--;
            if (key_equals(k, (size_t)(k_end - k), key)) {
                const char *v = eq + 1, *v_end = stop;
                while (v < v_end && (*v == ' ' || *v == '\t')) v++;
                while (v_end > v && (v_end[-1] == ' ' || v_end[-1] == '\t' || v_end[-1] == '\r')) v_end--;
                size_t n = (size_t)(v_end - v) < cap - 1 ? (size_t)(v_end - v) : cap - 1;
                /* a value cut short ends on a whole UTF-8 character */
                while (n > 0 && v + n < v_end && ((unsigned char)v[n] & 0xC0) == 0x80) n--;
                memcpy(out, v, n);
                out[n] = '\0';
                return true;
            }
        }
        line = *end ? end + 1 : end;
    }
    return false;
}

#define CONFIG_COMMENT_COL 23   /* column of the comments in mini_device_config_template */

/* Section name of a "[name]" line (trimmed), or false */
static bool section_line(const char *line, const char *end, const char **name, size_t *n) {
    while (line < end && (*line == ' ' || *line == '\t')) line++;
    if (line >= end || *line != '[') return false;
    const char *close = line;
    while (close < end && *close != ']') close++; /* no memchr in the WASM libc */
    if (close >= end) return false;
    *name = line + 1;
    *n = (size_t)(close - line - 1);
    return true;
}

char *mini_device_config_set(const char *text, const char *section, const char *key, const char *value) {
    if (!text) text = "";
    size_t tlen = strlen(text), vlen = strlen(value), klen = strlen(key), slen = strlen(section);
    char *out = (char *)malloc(tlen + vlen + klen + slen + 64);
    if (!out) return NULL;
    size_t insert_at = (size_t)-1;   /* end of the section's last non-empty line */
    bool in_section = false;
    for (const char *line = text; *line; ) {
        const char *end = strchr(line, '\n');
        if (!end) end = line + strlen(line);
        const char *name;
        size_t n;
        if (section_line(line, end, &name, &n)) {
            in_section = key_equals(name, n, section);
            if (in_section) insert_at = (size_t)((*end ? end + 1 : end) - text);
        } else {
            const char *stop = line;
            while (stop < end && *stop != ';' && *stop != '#') stop++;
            const char *eq = line;
            while (eq < stop && *eq != '=') eq++;
            if (eq < stop) {
                const char *k = line, *k_end = eq;
                while (k < k_end && (*k == ' ' || *k == '\t')) k++;
                while (k_end > k && (k_end[-1] == ' ' || k_end[-1] == '\t')) k_end--;
                if (key_equals(k, (size_t)(k_end - k), key)) {
                    /* "key = " kept; a comment starts at the template's column (23)
                     * when the value leaves room, else one space after it, so a
                     * long value that is replaced again leaves no gap behind */
                    const char *v = eq + 1;
                    while (v < stop && (*v == ' ' || *v == '\t')) v++;
                    if (v == stop) { /* empty value: the spaces before the comment are padding */
                        v = eq + 1;
                        if (v < stop && *v == ' ') v++;
                    }
                    size_t pre = (size_t)(v - text);
                    memcpy(out, text, pre);
                    size_t o = pre;
                    memcpy(out + o, value, vlen);
                    o += vlen;
                    if (stop < end && *stop != '\r') {
                        size_t col = (size_t)(v - line) + vlen;
                        size_t pad = col < CONFIG_COMMENT_COL ? CONFIG_COMMENT_COL - col : 1;
                        memset(out + o, ' ', pad);
                        o += pad;
                        memcpy(out + o, stop, tlen - (size_t)(stop - text));
                        o += tlen - (size_t)(stop - text);
                    } else {
                        const char *rest = stop;
                        while (rest < end && *rest != '\r') rest++;
                        memcpy(out + o, rest, tlen - (size_t)(rest - text));
                        o += tlen - (size_t)(rest - text);
                    }
                    out[o] = '\0';
                    return out;
                }
            }
            bool blank = true;
            for (const char *c = line; c < end; c++) {
                if (*c != ' ' && *c != '\t' && *c != '\r') { blank = false; break; }
            }
            if (in_section && !blank) insert_at = (size_t)((*end ? end + 1 : end) - text);
        }
        line = *end ? end + 1 : end;
    }
    size_t o;
    if (insert_at != (size_t)-1) {
        memcpy(out, text, insert_at);
        o = insert_at;
        if (o > 0 && out[o - 1] != '\n') out[o++] = '\n';
        o += (size_t)sprintf(out + o, "%s = %s\n", key, value);
        memcpy(out + o, text + insert_at, tlen - insert_at);
        o += tlen - insert_at;
    } else {
        memcpy(out, text, tlen);
        o = tlen;
        if (o > 0 && out[o - 1] != '\n') out[o++] = '\n';
        o += (size_t)sprintf(out + o, "\n[%s]\n%s = %s\n", section, key, value);
    }
    out[o] = '\0';
    return out;
}
