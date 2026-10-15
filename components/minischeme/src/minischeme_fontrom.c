/**
 * @file minischeme_fontrom.c
 * @brief System font ROM: MFNT fonts in a memory-mapped flash partition
 *        (layout in minischeme_fontrom.h).
 */

#include "minischeme_fontrom.h"
#include "minischeme_fonts.h"
#include "minischeme_gui.h"
#include "minischeme_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FR_SECTOR    MINI_FONTROM_SECTOR
#define FR_DATA      (2 * FR_SECTOR)
#define FR_HEADER    16
#define FR_ENTRY     32
#define FR_MAX       ((int)((FR_SECTOR - FR_HEADER) / FR_ENTRY))   /* entries one directory sector holds */
#define FR_CHUNK     2048   /* file read size when adding */

typedef struct {
    char name[MINI_FONTROM_NAME];
    uint32_t offset;
    uint32_t length;
} FrEntry;

static MiniFontRomFlash s_fr;
static bool s_on = false;
static FrEntry *s_dir = NULL;   /* the fonts, allocated for as many as there are */
static int s_dir_cap = 0;
static int s_n = 0;
static uint32_t s_seq = 0;
static int s_cur = -1;   /* directory sector in use, -1 for none */

static struct {
    bool on;
    char name[MINI_FONTROM_NAME];
    size_t start, len, done, erased;
} s_add;

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static size_t align_up(size_t n) {
    return (n + FR_SECTOR - 1) & ~(size_t)(FR_SECTOR - 1);
}

static uint32_t fnv(const uint8_t *p, size_t n, uint32_t h) {
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static bool name_char_ok(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '_' || c == '.';
}

static bool name_ok(const char *name) {
    size_t n = strlen(name);
    if (n == 0 || n >= MINI_FONTROM_NAME) return false;
    for (size_t i = 0; i < n; i++) {
        if (!name_char_ok(name[i])) return false;
    }
    return true;
}

/* ------------------------------------------------------------------------ */
/* Directory                                                                */
/* ------------------------------------------------------------------------ */

/* Room for n entries in s_dir */
static bool dir_room(int n) {
    if (n <= s_dir_cap) return true;
    FrEntry *d = (FrEntry *)realloc(s_dir, sizeof(FrEntry) * (size_t)n);
    if (!d) return false;
    s_dir = d;
    s_dir_cap = n;
    return true;
}

/* Directory sector k: its entry count, or -1 when it is not a valid directory */
static int dir_count(int k, uint32_t *seq) {
    const uint8_t *d = s_fr.map + (size_t)k * FR_SECTOR;
    if (memcmp(d, "MFRM", 4) != 0 || (d[4] | (d[5] << 8)) != 1) return -1;
    int count = d[6] | (d[7] << 8);
    if (count > FR_MAX) return -1;
    uint32_t h = fnv(d + 4, 8, 2166136261u);
    h = fnv(d + FR_HEADER, (size_t)count * FR_ENTRY, h);
    if (h != rd32(d + 12)) return -1;
    *seq = rd32(d + 8);
    return count;
}

/* The count entries of directory sector k into out */
static void dir_load(int k, FrEntry *out, int count) {
    const uint8_t *d = s_fr.map + (size_t)k * FR_SECTOR;
    for (int i = 0; i < count; i++) {
        const uint8_t *e = d + FR_HEADER + (size_t)i * FR_ENTRY;
        memcpy(out[i].name, e, MINI_FONTROM_NAME);
        out[i].name[MINI_FONTROM_NAME - 1] = '\0';
        out[i].offset = rd32(e + 24);
        out[i].length = rd32(e + 28);
    }
}

static bool dir_write(void) {
    int k = s_cur == 0 ? 1 : 0;
    size_t n = FR_HEADER + (size_t)s_n * FR_ENTRY;
    uint8_t *buf = (uint8_t *)calloc(1, n);
    if (!buf) return false;
    memcpy(buf, "MFRM", 4);
    buf[4] = 1;
    buf[6] = (uint8_t)s_n;
    buf[7] = (uint8_t)(s_n >> 8);
    wr32(buf + 8, s_seq + 1);
    for (int i = 0; i < s_n; i++) {
        uint8_t *e = buf + FR_HEADER + (size_t)i * FR_ENTRY;
        memcpy(e, s_dir[i].name, strlen(s_dir[i].name));
        wr32(e + 24, s_dir[i].offset);
        wr32(e + 28, s_dir[i].length);
    }
    uint32_t h = fnv(buf + 4, 8, 2166136261u);
    wr32(buf + 12, fnv(buf + FR_HEADER, (size_t)s_n * FR_ENTRY, h));
    bool ok = s_fr.erase((size_t)k * FR_SECTOR, FR_SECTOR, s_fr.user) &&
              s_fr.write((size_t)k * FR_SECTOR, buf, n, s_fr.user);
    free(buf);
    if (ok) {
        s_cur = k;
        s_seq++;
    }
    return ok;
}

static void dir_sort(void) {
    for (int i = 1; i < s_n; i++) {
        FrEntry e = s_dir[i];
        int j = i - 1;
        while (j >= 0 && s_dir[j].offset > e.offset) { s_dir[j + 1] = s_dir[j]; j--; }
        s_dir[j + 1] = e;
    }
}

static int dir_find(const char *name) {
    for (int i = 0; i < s_n; i++) {
        if (strcmp(s_dir[i].name, name) == 0) return i;
    }
    return -1;
}

static size_t data_end(void) {
    size_t end = FR_DATA;
    for (int i = 0; i < s_n; i++) {
        size_t e = align_up((size_t)s_dir[i].offset + s_dir[i].length);
        if (e > end) end = e;
    }
    return end;
}

void mini_fontrom_attach(const MiniFontRomFlash *flash) {
    s_on = false;
    s_n = 0;
    s_seq = 0;
    s_cur = -1;
    memset(&s_add, 0, sizeof(s_add));
    if (!flash || !flash->map || flash->size < FR_DATA + FR_SECTOR || !flash->erase || !flash->write) return;
    s_fr = *flash;
    s_fr.size &= ~(size_t)(FR_SECTOR - 1);
    uint32_t seq0 = 0, seq1 = 0;
    int n0 = dir_count(0, &seq0);
    int n1 = dir_count(1, &seq1);
    int k = (n1 >= 0 && (n0 < 0 || seq1 > seq0)) ? 1 : (n0 >= 0 ? 0 : -1);
    if (k >= 0) {
        int n = k ? n1 : n0;
        if (!dir_room(n)) return;
        dir_load(k, s_dir, n);
        s_n = n;
        s_seq = k ? seq1 : seq0;
        s_cur = k;
    }
    /* keep only entries that are whole, valid fonts (an interrupted move
     * leaves a damaged one; its space is reused by the next change) */
    int kept = 0;
    for (int i = 0; i < s_n; i++) {
        const FrEntry *e = &s_dir[i];
        bool ok = name_ok(e->name) && e->offset >= FR_DATA && e->offset % FR_SECTOR == 0 &&
                  (uint64_t)e->offset + e->length <= s_fr.size &&
                  mini_font_validate(s_fr.map + e->offset, e->length) == NULL;
        if (ok) s_dir[kept++] = *e;
    }
    s_n = kept;
    dir_sort();
    s_on = true;
}

bool mini_fontrom_present(void) {
    return s_on;
}

const uint8_t *mini_fontrom_find(const char *name, size_t *len) {
    if (!s_on || !name) return NULL;
    int i = dir_find(name);
    if (i < 0) return NULL;
    if (len) *len = s_dir[i].length;
    return s_fr.map + s_dir[i].offset;
}

int mini_fontrom_count(void) {
    return s_on ? s_n : 0;
}

const char *mini_fontrom_entry(int i, const uint8_t **data, size_t *len) {
    if (!s_on || i < 0 || i >= s_n) return NULL;
    if (data) *data = s_fr.map + s_dir[i].offset;
    if (len) *len = s_dir[i].length;
    return s_dir[i].name;
}

size_t mini_fontrom_used(void) {
    return s_on ? data_end() - FR_DATA : 0;
}

size_t mini_fontrom_capacity(void) {
    return s_on ? s_fr.size - FR_DATA : 0;
}

/* ------------------------------------------------------------------------ */
/* Changes: fonts reading the partition are rebuilt afterwards (by name,   */
/* so they follow moved fonts and pick up added ones)                       */
/* ------------------------------------------------------------------------ */

static void slots_release(void) {
    mini_font_release_range(s_fr.map, s_fr.map + s_fr.size);
}

static void slots_rebuild(void) {
    mini_font_rebuild();
}

/* Moves fonts down over the holes, one font at a time */
static const char *compact(void) {
    uint8_t *buf = NULL;
    size_t cursor = FR_DATA;
    for (int i = 0; i < s_n; i++) {
        FrEntry *e = &s_dir[i];
        if (e->offset > cursor) {
            if (!buf && !(buf = (uint8_t *)malloc(FR_SECTOR))) return "out of memory";
            size_t sectors = align_up(e->length) / FR_SECTOR;
            for (size_t k = 0; k < sectors; k++) {
                size_t src = e->offset + k * FR_SECTOR, dst = cursor + k * FR_SECTOR;
                memcpy(buf, s_fr.map + src, FR_SECTOR);
                if (!s_fr.erase(dst, FR_SECTOR, s_fr.user) || !s_fr.write(dst, buf, FR_SECTOR, s_fr.user)) {
                    free(buf);
                    return "flash write failed";
                }
            }
            e->offset = (uint32_t)cursor;
            if (!dir_write()) {
                free(buf);
                return "flash write failed";
            }
        }
        cursor = align_up((size_t)e->offset + e->length);
    }
    free(buf);
    return NULL;
}

const char *mini_fontrom_add_begin(const char *name, size_t len) {
    if (!s_on) return "no font ROM partition";
    s_add.on = false;
    if (name && name[0] && !name_ok(name)) return "font name: 1-23 letters, digits, '-', '_' or '.'";
    if (s_n >= FR_MAX && (!name || dir_find(name) < 0)) return "font ROM directory full";
    size_t start = data_end();
    if (len == 0 || start + len > s_fr.size) return "font ROM full";
    memset(&s_add, 0, sizeof(s_add));
    if (name) snprintf(s_add.name, sizeof(s_add.name), "%s", name);
    s_add.start = s_add.erased = start;
    s_add.len = len;
    s_add.on = true;
    return NULL;
}

const char *mini_fontrom_add_write(const void *data, size_t len) {
    if (!s_add.on) return "no font being added";
    if (s_add.done + len > s_add.len) {
        s_add.on = false;
        return "more data than announced";
    }
    size_t need = align_up(s_add.start + s_add.done + len);
    if (need > s_add.erased) {
        if (!s_fr.erase(s_add.erased, need - s_add.erased, s_fr.user)) {
            s_add.on = false;
            return "flash erase failed";
        }
        s_add.erased = need;
    }
    if (!s_fr.write(s_add.start + s_add.done, data, len, s_fr.user)) {
        s_add.on = false;
        return "flash write failed";
    }
    s_add.done += len;
    return NULL;
}

void mini_fontrom_add_abort(void) {
    s_add.on = false;
}

const char *mini_fontrom_add_end(void) {
    if (!s_add.on) return "no font being added";
    s_add.on = false;
    if (s_add.done != s_add.len) return "font data incomplete";
    const uint8_t *font = s_fr.map + s_add.start;
    const char *err = mini_font_validate(font, s_add.len);
    if (err) return err;
    char name[MINI_FONTROM_NAME];
    if (s_add.name[0]) {
        memcpy(name, s_add.name, sizeof(name));
    } else {
        /* the name stored in the font, cleaned up */
        size_t n = 0;
        for (size_t i = 0; i < 16 && font[16 + i] && n < sizeof(name) - 1; i++) {
            char c = (char)font[16 + i];
            name[n++] = name_char_ok(c) ? c : '_';
        }
        name[n] = '\0';
        if (!n) snprintf(name, sizeof(name), "font%d", s_n);
    }
    if (!dir_room(s_n + 1)) return "out of memory";
    slots_release();
    int old = dir_find(name);
    if (old >= 0) {
        memmove(&s_dir[old], &s_dir[old + 1], sizeof(FrEntry) * (size_t)(s_n - old - 1));
        s_n--;
    }
    FrEntry *e = &s_dir[s_n++];
    memset(e, 0, sizeof(*e));
    memcpy(e->name, name, sizeof(e->name));
    e->offset = (uint32_t)s_add.start;
    e->length = (uint32_t)s_add.len;
    err = dir_write() ? compact() : "flash write failed";
    slots_rebuild();
    return err;
}

const char *mini_fontrom_add_file(const char *path, const char *name) {
    if (!s_on) return "no font ROM partition";
    FILE *fp = path ? fopen(path, "rb") : NULL;
    if (!fp) {
        size_t len = 0;
        const uint8_t *data = mini_font_hook_read(path, &len);
        if (!data) return "cannot open the font file";
        const char *err = mini_fontrom_add_begin(name, len);
        if (!err) err = mini_fontrom_add_write(data, len);
        return err ? err : mini_fontrom_add_end();
    }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc(FR_CHUNK);
    const char *err = size <= 0 ? "empty font file" : (buf ? NULL : "out of memory");
    if (!err) err = mini_fontrom_add_begin(name, (size_t)size);
    for (size_t done = 0; !err && done < (size_t)size; ) {
        size_t n = (size_t)size - done < FR_CHUNK ? (size_t)size - done : FR_CHUNK;
        if (fread(buf, 1, n, fp) != n) err = "cannot read the font file";
        else err = mini_fontrom_add_write(buf, n);
        done += n;
    }
    fclose(fp);
    free(buf);
    if (err) {
        mini_fontrom_add_abort();
        return err;
    }
    return mini_fontrom_add_end();
}

const char *mini_fontrom_remove(const char *name) {
    if (!s_on) return "no font ROM partition";
    int i = name ? dir_find(name) : -1;
    if (i < 0) return "no such font";
    slots_release();
    memmove(&s_dir[i], &s_dir[i + 1], sizeof(FrEntry) * (size_t)(s_n - i - 1));
    s_n--;
    const char *err = dir_write() ? compact() : "flash write failed";
    slots_rebuild();
    return err;
}

/* ------------------------------------------------------------------------ */
/* Scheme primitives                                                        */
/* ------------------------------------------------------------------------ */

/* (fontrom-list) => (("name" height glyphs bytes) ...) */
static Value prim_fontrom_list(MiniVM *vm, int argc, Value *argv) {
    (void)argc; (void)argv;
    Value list = mini_nil(), item = mini_nil(), str = mini_nil();
    mini_push_root(vm, &list);
    mini_push_root(vm, &item);
    mini_push_root(vm, &str);
    for (int i = mini_fontrom_count() - 1; i >= 0; i--) {
        const uint8_t *d = NULL;
        size_t len = 0;
        const char *name = mini_fontrom_entry(i, &d, &len);
        item = mini_make_cons(vm, mini_make_fixnum((int64_t)len), mini_nil());
        item = mini_make_cons(vm, mini_make_fixnum((int64_t)rd32(d + 8)), item);
        item = mini_make_cons(vm, mini_make_fixnum(d[6]), item);
        str = mini_make_string(vm, name);
        item = mini_make_cons(vm, str, item);
        list = mini_make_cons(vm, item, list);
    }
    mini_pop_root(vm);
    mini_pop_root(vm);
    mini_pop_root(vm);
    return list;
}

/* (fontrom-info) => (used capacity count), #f without a font ROM */
static Value prim_fontrom_info(MiniVM *vm, int argc, Value *argv) {
    (void)argc; (void)argv;
    if (!mini_fontrom_present()) return mini_false();
    Value list = mini_nil();
    mini_push_root(vm, &list);
    list = mini_make_cons(vm, mini_make_fixnum(mini_fontrom_count()), list);
    list = mini_make_cons(vm, mini_make_fixnum((int64_t)mini_fontrom_capacity()), list);
    list = mini_make_cons(vm, mini_make_fixnum((int64_t)mini_fontrom_used()), list);
    mini_pop_root(vm);
    return list;
}

/* (fontrom-add path [name]) => name; replaces a font of the same name */
static Value prim_fontrom_add(MiniVM *vm, int argc, Value *argv) {
    if (!mini_access_privileged(vm, "fontrom-add")) return mini_void();
    if (argc < 1 || !mini_is_string(argv[0]) || (argc > 1 && !mini_is_string(argv[1]))) {
        vm_error(vm, "fontrom-add: expected path [name]");
        return mini_void();
    }
    char path[128], name[MINI_FONTROM_NAME + 8];
    snprintf(path, sizeof(path), "%s", mini_string_data(argv[0]));
    snprintf(name, sizeof(name), "%s", argc > 1 ? mini_string_data(argv[1]) : "");
    const char *err = mini_fontrom_add_file(path, name[0] ? name : NULL);
    if (err) {
        vm_error(vm, "fontrom-add: %s", err);
        return mini_void();
    }
    mini_gui_invalidate(0, mini_gui_get_height());
    /* the new font is the last one in the directory */
    const char *added = mini_fontrom_entry(mini_fontrom_count() - 1, NULL, NULL);
    return mini_make_string(vm, added ? added : "");
}

/* (fontrom-remove name) => #t, #f when there is no such font */
static Value prim_fontrom_remove(MiniVM *vm, int argc, Value *argv) {
    if (!mini_access_privileged(vm, "fontrom-remove")) return mini_void();
    if (argc < 1 || !mini_is_string(argv[0])) {
        vm_error(vm, "fontrom-remove: expected a font name");
        return mini_void();
    }
    char name[MINI_FONTROM_NAME + 8];
    snprintf(name, sizeof(name), "%s", mini_string_data(argv[0]));
    if (!mini_fontrom_find(name, NULL)) return mini_false();
    const char *err = mini_fontrom_remove(name);
    if (err) {
        vm_error(vm, "fontrom-remove: %s", err);
        return mini_void();
    }
    mini_gui_invalidate(0, mini_gui_get_height());
    return mini_true();
}

void mini_register_fontrom_lib(MiniVM *vm) {
    static const MiniPrimEntry funcs[] = {
        { "fontrom-list",   prim_fontrom_list },
        { "fontrom-info",   prim_fontrom_info },
        { "fontrom-add",    prim_fontrom_add },
        { "fontrom-remove", prim_fontrom_remove },
    };
    mini_register_prim_table(vm, funcs, sizeof(funcs) / sizeof(funcs[0]));
}
