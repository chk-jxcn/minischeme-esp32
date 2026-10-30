/**
 * @file minischeme_fontreg.c
 * @brief Font registry: every slot 0..7 holds a chain of sources searched
 *        glyph by glyph; the built-in fonts 0 and 1 are the last fallback.
 *
 * A slot is filled by a request: a name ("msyh-16", from use-font), the
 * MFNT bytes an app carries (font-register!), one MFNT image (C API) or one
 * file (font-load). Name and app requests are resolved into a chain from the
 * configured sources (config.ini [fonts], font-config):
 *   app      the bytes the app carries (Flash or RAM)
 *   fontrom  the system font ROM, font of that name (Flash, in place)
 *   spiffs   /spiffs/fonts/<name>.mfnt (RAM, or streamed when large)
 * font_override "name:other" puts the sources of `other` first; the
 * original name's sources stay behind it to fill gaps. A glyph no source has
 * is drawn with the system font.
 *
 * MFNT (tools/make_font.py), little-endian:
 *   0  "MFNT", u16 version (1 or 2), u8 height, u8 max bytes per row,
 *   8  u32 glyph count, u32 bitmap offset, char name[16]
 *   32 version 2: u8 bits per pixel (1, 2, 4), u8 ascent (baseline row),
 *      u8 descent, u8 flags, u32 identity hash, 8 reserved bytes
 *   index (32 in version 1, 48 in version 2), sorted by code point, 8 bytes:
 *      u16 code point, u8 advance, u8 bytes per row, u32 bitmap offset
 *      (bytes per row and advance 0: the face has no such glyph)
 *   bitmaps: height rows, MSB = leftmost pixel; 2 and 4 bits per pixel hold
 *   coverage (anti-aliasing)
 *
 * A font in memory (Flash, a Code ROM string literal, or a RAM copy) is used
 * in place. A font in a file stays on disk: RAM holds the first code point of
 * every block of 64 index entries, so a glyph costs one block read and one
 * bitmap read, and glyphs pass through a small cache.
 */

#include "minischeme_fonts.h"
#include "minischeme_fontnet.h"
#include "minischeme_fontrom.h"
#include "minischeme_gui.h"
#include "minischeme_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MFNT_HEADER_V1  32
#define MFNT_HEADER_V2  48
#define MFNT_ENTRY      8
#define MFNT_MISSES     16   /* code points known to be absent (file fonts) */
#define MFNT_BLOCK      64   /* index entries per block read (file fonts) */
#define CHAIN_MAX       8    /* sources per slot */
#define SOURCE_POOL     24
#define NAME_MAX_LEN    24

#if defined(ESP_PLATFORM)
#define MINI_FONT_RAM_LIMIT_DEFAULT (24 * 1024) /* larger font files are streamed */
#else
#define MINI_FONT_RAM_LIMIT_DEFAULT (8 * 1024 * 1024)
#endif
/* glyph cache shared by every font; the WebREPL simulator (and the device
 * parity tests) keep the device's size so glyphs are evicted as on the board */
#ifndef MINI_FONT_GCACHE_BYTES
#if defined(ESP_PLATFORM) || defined(__wasm__)
#define MINI_FONT_GCACHE_BYTES   (8 * 1024)
#define MINI_FONT_GCACHE_ENTRIES 96
#else
#define MINI_FONT_GCACHE_BYTES   (256 * 1024)
#define MINI_FONT_GCACHE_ENTRIES 1024
#endif
#endif
#define GCACHE_BYTES   MINI_FONT_GCACHE_BYTES
#define GCACHE_ENTRIES MINI_FONT_GCACHE_ENTRIES
#ifndef MINI_FONT_RAM_LIMIT
#define MINI_FONT_RAM_LIMIT MINI_FONT_RAM_LIMIT_DEFAULT
#endif

typedef enum { K_NONE, K_MEM, K_FILE } SourceKind;

typedef struct {
    MiniFont font;          /* first member: MiniFont pointers map back to the source */
    char name[NAME_MAX_LEN];
    SourceKind kind;
    const char *label;      /* "rom", "ram", "file", "fontrom" */
    /* MFNT in memory or in a file */
    const uint8_t *data;
    uint8_t *owned;         /* RAM copy to free */
    size_t bytes;
    FILE *fp;
    uint32_t index_off;
    uint32_t bitmap_off;
    uint8_t max_bpr;
    uint8_t bpp;
    uint16_t *block_first;  /* file fonts: first code point of each index block */
    uint32_t nblocks;
    uint8_t *block;
    int32_t block_no;
    uint32_t miss[MFNT_MISSES];
    uint8_t nmiss, next_miss;
    /* memory fonts: rotating glyph views pointing into the data (the other
     * kinds copy their bitmaps into the shared glyph cache) */
    MiniGlyph views[4];
    int next;
} Source;

typedef enum { REQ_NONE, REQ_RAW, REQ_FILE, REQ_NAME, REQ_APP } RequestKind;

typedef struct {
    RequestKind kind;
    char name[NAME_MAX_LEN];   /* REQ_NAME / REQ_APP */
    const uint8_t *data;       /* REQ_RAW / REQ_APP */
    size_t len;
    uint8_t *owned;            /* copy of data */
    const char *label;         /* REQ_RAW */
    char *path;                /* REQ_FILE (allocated) */
    size_t ram_limit;
} Request;

typedef struct {
    MiniFont font;             /* what mini_font_get returns for the slot */
    char name[NAME_MAX_LEN];
    Source *src[CHAIN_MAX];
    int n;
} Chain;

static Request s_req[MINI_FONT_SLOTS];
static Chain s_chain[MINI_FONT_SLOTS];
static Source *s_pool[SOURCE_POOL];   /* allocated per source, freed with it */
static MiniFontFileHook s_file_hook = NULL;

/* config.ini [fonts] */
enum { SRC_APP, SRC_FONTROM, SRC_SPIFFS, SRC_COUNT };
static const char *const s_src_names[SRC_COUNT] = { "app", "fontrom", "spiffs" };
static uint8_t s_order[SRC_COUNT] = { SRC_APP, SRC_FONTROM, SRC_SPIFFS };
static int s_norder = SRC_COUNT;
#define FONT_OVERRIDES 8
typedef struct {
    char from[NAME_MAX_LEN];
    char to[NAME_MAX_LEN];
} FontOverride;
static FontOverride s_over[FONT_OVERRIDES];
static int s_nover = 0;

void mini_font_set_file_hook(MiniFontFileHook hook) {
    s_file_hook = hook;
}

const uint8_t *mini_font_hook_read(const char *path, size_t *len) {
    return s_file_hook && path ? s_file_hook(path, len) : NULL;
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

/* ========================================================================= */
/* Sources                                                                   */
/* ========================================================================= */

/* ========================================================================= */
/* Glyph cache                                                               */
/* ========================================================================= */

/* Bitmaps that are not in memory (streamed files, downloads, rasterized
 * outlines) are copied into one ring buffer shared by every font, so the RAM
 * they take is bounded however many fonts are in use. A returned glyph stays
 * valid until the next lookup at least, which is how the text loop uses it. */
typedef struct {
    const void *src;        /* NULL: free */
    uint32_t start, len;    /* bytes in the ring */
    MiniGlyph g;
} GEntry;

static uint8_t *s_gc_buf = NULL;
static GEntry *s_gc = NULL;   /* GCACHE_ENTRIES, allocated with the buffer on first use */
static uint32_t s_gc_head = 0;
static int s_gc_next = 0;

static MiniGlyph *gcache_find(const void *src, uint32_t cp) {
    if (!s_gc) return NULL;
    for (int i = 0; i < GCACHE_ENTRIES; i++) {
        if (s_gc[i].src == src && s_gc[i].g.codepoint == cp) return &s_gc[i].g;
    }
    return NULL;
}

static void gcache_forget(const void *src) {
    if (!s_gc) return;
    for (int i = 0; i < GCACHE_ENTRIES; i++) {
        if (s_gc[i].src == src) s_gc[i].src = NULL;
    }
}

/* Room for up to cap bytes of a new glyph; gcache_commit then keeps the
 * bytes really used. NULL when cap does not fit or there is no RAM. */
static GEntry *gcache_reserve(uint32_t cp, size_t cap, uint8_t **dst) {
    if (!s_gc) s_gc = (GEntry *)calloc(GCACHE_ENTRIES, sizeof(GEntry));
    if (s_gc && !s_gc_buf) s_gc_buf = (uint8_t *)malloc(GCACHE_BYTES);
    if (!s_gc || !s_gc_buf || cap == 0 || cap > GCACHE_BYTES / 2) return NULL;
    if (s_gc_head + cap > GCACHE_BYTES) s_gc_head = 0;
    uint32_t lo = s_gc_head, hi = s_gc_head + (uint32_t)cap;
    for (int i = 0; i < GCACHE_ENTRIES; i++) {
        if (s_gc[i].src && s_gc[i].start < hi && s_gc[i].start + s_gc[i].len > lo) s_gc[i].src = NULL;
    }
    GEntry *e = &s_gc[s_gc_next];
    s_gc_next = (s_gc_next + 1) % GCACHE_ENTRIES;
    memset(e, 0, sizeof(*e));
    e->start = lo;
    e->g.codepoint = cp;
    *dst = s_gc_buf + lo;
    return e;
}

static MiniGlyph *gcache_commit(GEntry *e, const void *src, size_t used) {
    e->src = src;
    e->len = (uint32_t)used;
    e->g.bitmap = s_gc_buf + e->start;
    s_gc_head = e->start + (uint32_t)used;
    return &e->g;
}

static Source *source_alloc(void) {
    for (int i = 0; i < SOURCE_POOL; i++) {
        if (!s_pool[i]) return s_pool[i] = (Source *)calloc(1, sizeof(Source));
    }
    return NULL;
}

static void source_free(Source *s) {
    if (!s) return;
    if (s->fp) fclose(s->fp);
    free(s->owned);
    gcache_forget(s);
    free(s->block_first);
    free(s->block);
    for (int i = 0; i < SOURCE_POOL; i++) {
        if (s_pool[i] == s) s_pool[i] = NULL;
    }
    free(s);
}

static Source *source_of(const MiniFont *font) {
    if (!font || font->glyphs) return NULL;
    for (int i = 0; i < SOURCE_POOL; i++) {
        if (s_pool[i] && &s_pool[i]->font == font) return s_pool[i]->kind != K_NONE ? s_pool[i] : NULL;
    }
    return NULL;
}

static Chain *chain_of(const MiniFont *font) {
    for (int i = 0; i < MINI_FONT_SLOTS; i++) {
        if (&s_chain[i].font == font) return &s_chain[i];
    }
    return NULL;
}

/* Header checks; fills the source's metrics */
static const char *parse_header(Source *s, const uint8_t *h, size_t total) {
    if (total < MFNT_HEADER_V1 || memcmp(h, "MFNT", 4) != 0) return "not an MFNT font";
    int version = rd16(h + 4);
    if (version != 1 && version != 2) return "unsupported MFNT version";
    uint32_t index_off = version == 1 ? MFNT_HEADER_V1 : MFNT_HEADER_V2;
    if (total < index_off) return "truncated MFNT font";
    uint8_t bpp = version == 1 ? 1 : h[32];
    if (bpp != 1 && bpp != 2 && bpp != 4) return "bad MFNT bits per pixel";
    uint32_t count = rd32(h + 8);
    uint32_t bitmap_off = rd32(h + 12);
    if (h[6] == 0 || h[6] > 128 || h[7] == 0 || h[7] > 16 * bpp) return "bad MFNT metrics";
    if ((uint64_t)index_off + (uint64_t)count * MFNT_ENTRY > bitmap_off || bitmap_off > total) {
        return "truncated MFNT font";
    }
    memcpy(s->name, h + 16, 16);
    s->name[16] = '\0';
    s->font.name = s->name;
    s->font.height = h[6];
    s->font.glyph_count = (int)count;
    s->font.glyphs = NULL;
    s->font.ascent = version == 1 ? 0 : h[33];
    s->index_off = index_off;
    s->bitmap_off = bitmap_off;
    s->max_bpr = h[7];
    s->bpp = bpp;
    s->bytes = total;
    return NULL;
}

/* Index entries checked: sorted, bitmaps inside the data */
static const char *check_entry(const Source *s, const uint8_t *e, uint64_t total) {
    if (e[3] == 0) return e[2] == 0 ? NULL : "MFNT glyph outside the font data";
    uint64_t end = (uint64_t)s->bitmap_off + rd32(e + 4) + (uint64_t)e[3] * s->font.height;
    if (e[3] > s->max_bpr || end > total) return "MFNT glyph outside the font data";
    return NULL;
}

static const char *mfnt_check(Source *tmp, const uint8_t *data, size_t len) {
    if (!data) return "no font data";
    const char *err = parse_header(tmp, data, len);
    if (err) return err;
    uint32_t count = (uint32_t)tmp->font.glyph_count;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = data + tmp->index_off + (size_t)i * MFNT_ENTRY;
        if ((err = check_entry(tmp, e, len)) != NULL) return err;
        if (i > 0 && rd16(e) <= rd16(e - MFNT_ENTRY)) return "MFNT index not sorted";
    }
    return NULL;
}

const char *mini_font_validate(const uint8_t *data, size_t len) {
    Source tmp;
    memset(&tmp, 0, sizeof(tmp));
    return mfnt_check(&tmp, data, len);
}

static Source *source_mem(const uint8_t *data, size_t len, bool copy, const char *label, const char **err_out) {
    Source tmp;
    memset(&tmp, 0, sizeof(tmp));
    const char *err = mfnt_check(&tmp, data, len);
    if (err) {
        if (err_out) *err_out = err;
        return NULL;
    }
    Source *s = source_alloc();
    if (!s) {
        if (err_out) *err_out = "too many fonts";
        return NULL;
    }
    *s = tmp;
    s->font.name = s->name;
    s->kind = K_MEM;
    if (copy) {
        s->owned = (uint8_t *)malloc(len);
        if (!s->owned) {
            source_free(s);
            if (err_out) *err_out = "out of memory for the font";
            return NULL;
        }
        memcpy(s->owned, data, len);
    }
    s->data = s->owned ? s->owned : data;
    s->label = label ? label : (s->owned ? "ram" : "rom");
    return s;
}

static Source *source_file(const char *path, size_t ram_limit, const char **err_out) {
    const char *dummy;
    const char **err = err_out ? err_out : &dummy;
    FILE *fp = path ? fopen(path, "rb") : NULL;
    if (!fp && s_file_hook && path) {
        size_t len = 0;
        const uint8_t *data = s_file_hook(path, &len);
        if (data) return source_mem(data, len, true, "ram", err_out);
    }
    if (!fp) {
        *err = "cannot open the font file";
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size <= 0) {
        fclose(fp);
        *err = "empty font file";
        return NULL;
    }
    if ((size_t)size <= ram_limit) {
        uint8_t *buf = (uint8_t *)malloc((size_t)size);
        bool ok = buf && fread(buf, 1, (size_t)size, fp) == (size_t)size;
        fclose(fp);
        if (!ok) {
            free(buf);
            *err = buf ? "cannot read the font file" : "out of memory for the font";
            return NULL;
        }
        Source *s = source_mem(buf, (size_t)size, false, "ram", err_out);
        if (!s) {
            free(buf);
            return NULL;
        }
        s->owned = buf;
        return s;
    }
    /* Too big for RAM: keep the file open, cache glyphs. Unbuffered: every
     * read below is one block or one bitmap, a single file system call. */
    setvbuf(fp, NULL, _IONBF, 0);
    uint8_t h[MFNT_HEADER_V2];
    Source tmp;
    memset(&tmp, 0, sizeof(tmp));
    size_t got = fread(h, 1, sizeof(h), fp);
    const char *e = got >= MFNT_HEADER_V1 ? parse_header(&tmp, h, (size_t)size) : "truncated MFNT font";
    if (!e && got < tmp.index_off) e = "truncated MFNT font";
    if (e) {
        fclose(fp);
        *err = e;
        return NULL;
    }
    fseek(fp, (long)tmp.index_off, SEEK_SET);
    uint32_t count = (uint32_t)tmp.font.glyph_count;
    tmp.nblocks = (count + MFNT_BLOCK - 1) / MFNT_BLOCK;
    tmp.block_first = (uint16_t *)malloc((tmp.nblocks ? tmp.nblocks : 1) * sizeof(uint16_t));
    tmp.block = (uint8_t *)malloc(MFNT_BLOCK * MFNT_ENTRY);
    tmp.block_no = -1;
    e = tmp.block_first && tmp.block ? NULL : "out of memory for the font index";
    /* one sequential pass: block table, plus the checks memory fonts get */
    uint32_t prev = 0;
    for (uint32_t b = 0; !e && b < tmp.nblocks; b++) {
        uint32_t n = count - b * MFNT_BLOCK < MFNT_BLOCK ? count - b * MFNT_BLOCK : MFNT_BLOCK;
        if (fread(tmp.block, MFNT_ENTRY, n, fp) != n) {
            e = "truncated MFNT font";
            break;
        }
        for (uint32_t i = 0; i < n && !e; i++) {
            const uint8_t *en = tmp.block + i * MFNT_ENTRY;
            e = check_entry(&tmp, en, (uint64_t)size);
            if (!e && (b > 0 || i > 0) && rd16(en) <= prev) e = "MFNT index not sorted";
            prev = rd16(en);
        }
        tmp.block_first[b] = rd16(tmp.block);
    }
    if (e) {
        fclose(fp);
        free(tmp.block_first);
        free(tmp.block);
        *err = e;
        return NULL;
    }
    Source *s = source_alloc();
    if (!s) {
        fclose(fp);
        free(tmp.block_first);
        free(tmp.block);
        *err = "too many fonts";
        return NULL;
    }
    *s = tmp;
    s->font.name = s->name;
    s->kind = K_FILE;
    s->fp = fp;
    s->label = "file";
    return s;
}

/* ========================================================================= */
/* Glyph lookup                                                              */
/* ========================================================================= */

/* Binary search of n sorted index entries; the entry or NULL */
static const uint8_t *index_search(const uint8_t *entries, uint32_t n, uint32_t cp) {
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint16_t c = rd16(entries + (size_t)mid * MFNT_ENTRY);
        if (c == cp) return entries + (size_t)mid * MFNT_ENTRY;
        if (c < cp) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}

/* Index entry of cp in a file font: one block read at most */
static const uint8_t *file_entry(Source *s, uint32_t cp) {
    uint32_t lo = 0, hi = s->nblocks; /* last block whose first code point <= cp */
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo + 1) / 2;
        if (mid < s->nblocks && s->block_first[mid] <= cp) lo = mid;
        else hi = mid - 1;
    }
    if (s->nblocks == 0 || s->block_first[lo] > cp) return NULL;
    uint32_t count = (uint32_t)s->font.glyph_count;
    uint32_t n = count - lo * MFNT_BLOCK < MFNT_BLOCK ? count - lo * MFNT_BLOCK : MFNT_BLOCK;
    if (s->block_no != (int32_t)lo) {
        s->block_no = -1;
        long off = (long)(s->index_off + (size_t)lo * MFNT_BLOCK * MFNT_ENTRY);
        if (fseek(s->fp, off, SEEK_SET) != 0 || fread(s->block, MFNT_ENTRY, n, s->fp) != n) return NULL;
        s->block_no = (int32_t)lo;
    }
    return index_search(s->block, n, cp);
}

static bool known_miss(Source *s, uint32_t cp) {
    for (int i = 0; i < s->nmiss; i++) {
        if (s->miss[i] == cp) return true;
    }
    return false;
}

static void note_miss(Source *s, uint32_t cp) {
    s->miss[s->next_miss] = cp;
    s->next_miss = (uint8_t)((s->next_miss + 1) % MFNT_MISSES);
    if (s->nmiss < MFNT_MISSES) s->nmiss++;
}

static MiniGlyph *next_view(Source *s) {
    MiniGlyph *g = &s->views[s->next];
    s->next = (s->next + 1) % 4;
    g->bitmap = NULL;
    return g;
}

static const MiniGlyph *source_find(Source *s, uint32_t cp) {
    if (!s || s->kind == K_NONE || cp > 0xFFFF) return NULL;   /* MFNT holds 16-bit code points */
    MiniGlyph *g;
    uint8_t *dst;
    GEntry *ce;
    const uint8_t *e;
    if (s->fp) {
        if ((g = gcache_find(s, cp)) != NULL) return g;
        if (known_miss(s, cp)) return NULL;
        e = file_entry(s, cp);
        if (!e || e[3] == 0) {
            note_miss(s, cp);
            return NULL;
        }
        /* entries were checked at load: bytes per row <= max_bpr */
        size_t n = (size_t)e[3] * s->font.height;
        size_t bm = (size_t)s->bitmap_off + rd32(e + 4);
        if ((ce = gcache_reserve(cp, n, &dst)) == NULL) return NULL;
        if (fseek(s->fp, (long)bm, SEEK_SET) != 0 || fread(dst, 1, n, s->fp) != n) return NULL;
        g = gcache_commit(ce, s, n);
        g->advance = e[2];
        g->bytes_per_row = e[3];
        g->bpp = s->bpp;
        return g;
    }
    e = index_search(s->data + s->index_off, (uint32_t)s->font.glyph_count, cp);
    if (!e || e[3] == 0) return NULL;
    g = next_view(s);
    g->codepoint = cp;
    g->advance = e[2];
    g->bytes_per_row = e[3];
    g->bpp = s->bpp;
    g->bitmap = s->data + (size_t)s->bitmap_off + rd32(e + 4);
    return g;
}

/* Every source of the chain, in order */
static const MiniGlyph *chain_find(Chain *c, uint32_t cp, const MiniFont **used) {
    for (int i = 0; i < c->n; i++) {
        const MiniGlyph *g = source_find(c->src[i], cp);
        if (g) {
            if (used) *used = &c->src[i]->font;
            return g;
        }
    }
    return NULL;
}

static const MiniGlyph *builtin_find(const MiniFont *font, uint32_t cp) {
    if (!font || !font->glyphs) return NULL;
    int low = 0, high = font->glyph_count - 1;
    while (low <= high) {
        int mid = (low + high) / 2;
        if (font->glyphs[mid].codepoint == cp) return &font->glyphs[mid];
        if (font->glyphs[mid].codepoint < cp) low = mid + 1;
        else high = mid - 1;
    }
    return NULL;
}

const MiniGlyph *mini_font_find_glyph(const MiniFont *font, uint32_t codepoint) {
    if (!font) return NULL;
    Chain *c = chain_of(font);
    if (c) return chain_find(c, codepoint, NULL);
    Source *s = source_of(font);
    if (s) return source_find(s, codepoint);
    return builtin_find(font, codepoint);
}

/* A loaded slot, else the built-in font, else the system font (maybe replaced) */
const MiniFont *mini_font_get(int font_id) {
    if (font_id >= 0 && font_id < MINI_FONT_SLOTS && s_chain[font_id].n > 0) return &s_chain[font_id].font;
    const MiniFont *f = mini_font_builtin(font_id);
    if (f) return f;
    return mini_font_get(MINI_FONT_SYSTEM_16);
}

static const MiniGlyph *font_lookup(const MiniFont *font, uint32_t cp, const MiniFont **used) {
    Chain *c = chain_of(font);
    if (c) return chain_find(c, cp, used);
    const MiniGlyph *g = mini_font_find_glyph(font, cp);
    if (g && used) *used = font;
    return g;
}

/* The font (its whole chain), then the system font (font 0, maybe
 * replaced), then the built-in 16 px font, then the glyphs downloaded from
 * the font server (a glyph no one has yet is asked for there) */
/* Text in the system font (font 0, or the built-in one it replaces) */
static bool is_system_font(const MiniFont *font) {
    return font == mini_font_get(MINI_FONT_SYSTEM_16) || font == mini_font_builtin(MINI_FONT_SYSTEM_16);
}

const MiniGlyph *mini_font_glyph(const MiniFont *font, uint32_t codepoint, const MiniFont **used) {
    /* font_server_first: the server's face for system-font text; the
     * board's glyph stands in until the server's has arrived */
    if (mini_fontnet_first() && is_system_font(font)) {
        const MiniGlyph *g = mini_fontnet_glyph(codepoint, used);
        if (g) return g;
    }
    const MiniFont *chain[3] = { font, mini_font_get(MINI_FONT_SYSTEM_16), mini_font_builtin(MINI_FONT_SYSTEM_16) };
    for (int i = 0; i < 3; i++) {
        if (!chain[i] || (i > 0 && (chain[i] == chain[0] || chain[i] == chain[i - 1]))) continue;
        const MiniGlyph *g = font_lookup(chain[i], codepoint, used);
        if (g) return g;
    }
    return codepoint >= 0x20 ? mini_fontnet_glyph(codepoint, used) : NULL;
}

/* Width text layout uses: a missing glyph takes the replaced system font's
 * advance when there is one, else 8 (ASCII) or the font height */
int mini_font_advance(const MiniFont *font, uint32_t codepoint) {
    if (mini_fontnet_first() && is_system_font(font)) {
        int net = mini_fontnet_advance(codepoint);
        if (net >= 0) return net;
    }
    const MiniGlyph *g = font_lookup(font, codepoint, NULL);
    if (g) return g->advance;
    const MiniFont *sys = mini_font_get(MINI_FONT_SYSTEM_16);
    if (s_chain[MINI_FONT_SYSTEM_16].n > 0 && font != sys) {
        g = font_lookup(sys, codepoint, NULL);
        if (g) return g->advance;
    }
    int net = codepoint >= 128 ? mini_fontnet_advance(codepoint) : -1;
    if (net >= 0) return net;
    if (mini_font_ignorable(codepoint)) return 0;
    return codepoint < 128 ? 8 : font->height;
}

uint32_t mini_utf8_next(const unsigned char **pp) {
    const unsigned char *p = *pp;
    unsigned char c = *p++;
    uint32_t cp = c;
    if ((c & 0xF8) == 0xF0 && (p[0] & 0xC0) == 0x80 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        cp = ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(p[0] & 0x3F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        p += 3;
    } else if ((c & 0xF0) == 0xE0 && (p[0] & 0xC0) == 0x80 && (p[1] & 0xC0) == 0x80) {
        cp = ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(p[0] & 0x3F) << 6) | (p[1] & 0x3F);
        p += 2;
    } else if ((c & 0xE0) == 0xC0 && (p[0] & 0xC0) == 0x80) {
        cp = ((uint32_t)(c & 0x1F) << 6) | (p[0] & 0x3F);
        p += 1;
    }
    *pp = p;
    return cp;
}

int mini_font_text_width(int font_id, const char *utf8_str) {
    if (!utf8_str) return 0;
    const MiniFont *font = mini_font_get(font_id);
    int total_w = 0;
    const unsigned char *p = (const unsigned char *)utf8_str;
    while (*p) total_w += mini_font_advance(font, mini_utf8_next(&p));
    return total_w;
}

/* ========================================================================= */
/* Requests and chains                                                       */
/* ========================================================================= */

static void chain_clear(int id) {
    Chain *c = &s_chain[id];
    for (int i = 0; i < c->n; i++) source_free(c->src[i]);
    memset(c, 0, sizeof(*c));
}

static void request_clear(int id) {
    free(s_req[id].owned);
    free(s_req[id].path);
    memset(&s_req[id], 0, sizeof(Request));
}

static void chain_add(Chain *c, Source *s) {
    if (!s) return;
    if (c->n == CHAIN_MAX) {
        source_free(s);
        return;
    }
    c->src[c->n++] = s;
}

static const char *override_for(const char *name) {
    for (int i = 0; i < s_nover; i++) {
        if (strcmp(s_over[i].from, name) == 0) return s_over[i].to;
    }
    return NULL;
}

/* "msyh-16" -> face "msyh", size 16; false without a size suffix */
static bool split_name(const char *name, char *face, size_t cap, int *size) {
    const char *dash = strrchr(name, '-');
    if (!dash || !dash[1]) return false;
    for (const char *p = dash + 1; *p; p++) {
        if (*p < '0' || *p > '9') return false;
    }
    int n = (int)strtoll(dash + 1, NULL, 10);
    size_t flen = (size_t)(dash - name);
    if (n < 6 || n > 128 || flen == 0 || flen >= cap) return false;
    memcpy(face, name, flen);
    face[flen] = '\0';
    *size = n;
    return true;
}

/* Sources of one font name, in font_order (app: the request's bytes) */
static void add_named(Chain *c, const char *name, const Request *r, bool with_app) {
    for (int i = 0; i < s_norder; i++) {
        switch (s_order[i]) {
            case SRC_APP:
                if (with_app && r->data) chain_add(c, source_mem(r->data, r->len, false, r->owned ? "ram" : "rom", NULL));
                break;
            case SRC_FONTROM: {
                size_t len = 0;
                const uint8_t *d = mini_fontrom_find(name, &len);
                if (d) chain_add(c, source_mem(d, len, false, "fontrom", NULL));
                break;
            }
            case SRC_SPIFFS: {
                char path[64];
                snprintf(path, sizeof(path), "/spiffs/fonts/%s.mfnt", name);
                chain_add(c, source_file(path, MINI_FONT_RAM_LIMIT, NULL));
                break;
            }
            default:
                break;
        }
    }
}

static void chain_build(int id) {
    chain_clear(id);
    Request *r = &s_req[id];
    Chain *c = &s_chain[id];
    switch (r->kind) {
        case REQ_RAW:
            chain_add(c, source_mem(r->data, r->len, false, r->label ? r->label : (r->owned ? "ram" : "rom"), NULL));
            break;
        case REQ_FILE:
            chain_add(c, source_file(r->path, r->ram_limit, NULL));
            break;
        case REQ_NAME:
        case REQ_APP: {
            const char *to = override_for(r->name);
            if (to) add_named(c, to, r, false);
            add_named(c, r->name, r, true);
            break;
        }
        default:
            break;
    }
    if (c->n == 0) return;
    const Source *first = c->src[0];
    snprintf(c->name, sizeof(c->name), "%s", first->name);
    c->font.id = id;
    c->font.name = c->name;
    c->font.glyphs = NULL;
    c->font.glyph_count = first->font.glyph_count;
    c->font.ascent = first->font.ascent;
    int size = 0;
    char face[NAME_MAX_LEN];
    /* a named request is drawn at its size, whatever its first source */
    if ((r->kind == REQ_NAME || r->kind == REQ_APP) && split_name(override_for(r->name) ? override_for(r->name) : r->name,
                                                                   face, sizeof(face), &size)) {
        c->font.height = (uint8_t)size;
    } else {
        c->font.height = first->font.height;
    }
    if (c->font.height != first->font.height) c->font.ascent = 0;
}

/* Built-in slots 0 and 1 follow font_override */
static void builtin_requests(void) {
    for (int id = 0; id < 2; id++) {
        const MiniFont *b = mini_font_builtin(id);
        bool replaced = b && override_for(b->name);
        if (replaced && s_req[id].kind == REQ_NONE) {
            s_req[id].kind = REQ_NAME;
            snprintf(s_req[id].name, sizeof(s_req[id].name), "%s", b->name);
        } else if (!replaced && s_req[id].kind == REQ_NAME && b && strcmp(s_req[id].name, b->name) == 0) {
            request_clear(id);
        }
    }
}

const char *mini_font_system_face(int *height) {
    static char face[NAME_MAX_LEN];
    const Chain *c = &s_chain[MINI_FONT_SYSTEM_16];
    int size = 0;
    if (c->n > 0 && split_name(c->name, face, sizeof(face), &size)) {
        if (height) *height = c->font.height ? c->font.height : size;
        return face;
    }
    if (height) *height = mini_font_builtin(MINI_FONT_SYSTEM_16)->height;
    return "system";
}

void mini_font_rebuild(void) {
    builtin_requests();
    for (int id = 0; id < MINI_FONT_SLOTS; id++) chain_build(id);
}

void mini_font_release_range(const uint8_t *lo, const uint8_t *hi) {
    for (int id = 0; id < MINI_FONT_SLOTS; id++) {
        Chain *c = &s_chain[id];
        for (int i = 0; i < c->n; i++) {
            const uint8_t *d = c->src[i]->data;
            if (d && d >= lo && d < hi) {
                chain_clear(id);
                break;
            }
        }
    }
}

void mini_font_unload(int font_id) {
    if (font_id < 0 || font_id >= MINI_FONT_SLOTS) return;
    chain_clear(font_id);
    request_clear(font_id);
}

static const char *request_raw(int font_id, const uint8_t *data, size_t len, bool copy, const char *label, RequestKind kind) {
    if (font_id < 0 || font_id >= MINI_FONT_SLOTS) return "font id must be 0..7";
    const char *err = mini_font_validate(data, len);
    if (err) return err;
    uint8_t *owned = NULL;
    if (copy) {
        owned = (uint8_t *)malloc(len);
        if (!owned) return "out of memory for the font";
        memcpy(owned, data, len);
    }
    mini_font_unload(font_id);
    Request *r = &s_req[font_id];
    r->kind = kind;
    r->owned = owned;
    r->data = owned ? owned : data;
    r->len = len;
    r->label = label;
    memcpy(r->name, data + 16, 16);
    r->name[16] = '\0';
    chain_build(font_id);
    if (s_chain[font_id].n == 0 && kind == REQ_RAW) {
        request_clear(font_id);
        return "out of memory for the font";
    }
    return NULL;
}

const char *mini_font_register(int font_id, const uint8_t *data, size_t len, bool copy) {
    return mini_font_register_ex(font_id, data, len, copy, NULL);
}

const char *mini_font_register_ex(int font_id, const uint8_t *data, size_t len, bool copy, const char *source) {
    return request_raw(font_id, data, len, copy, source, REQ_RAW);
}

const char *mini_font_load_file(int font_id, const char *path, size_t ram_limit) {
    if (font_id < 0 || font_id >= MINI_FONT_SLOTS) return "font id must be 0..7";
    const char *err = NULL;
    Source *probe = source_file(path, ram_limit, &err);
    if (!probe) return err;
    char *copy = (char *)malloc(strlen(path) + 1);
    if (!copy) {
        source_free(probe);
        return "out of memory";
    }
    strcpy(copy, path);
    mini_font_unload(font_id);
    Request *r = &s_req[font_id];
    r->kind = REQ_FILE;
    r->path = copy;
    r->ram_limit = ram_limit;
    Chain *c = &s_chain[font_id];
    chain_add(c, probe);
    c->font.id = font_id;
    snprintf(c->name, sizeof(c->name), "%s", probe->name);
    c->font.name = c->name;
    c->font.height = probe->font.height;
    c->font.ascent = probe->font.ascent;
    c->font.glyph_count = probe->font.glyph_count;
    return NULL;
}

/* use-font: a font by name, resolved through every configured source */
static bool request_name(int id, const char *name) {
    mini_font_unload(id);
    s_req[id].kind = REQ_NAME;
    snprintf(s_req[id].name, sizeof(s_req[id].name), "%s", name);
    chain_build(id);
    return s_chain[id].n > 0;
}

bool mini_font_use(int font_id, const char *face, int size) {
    if (font_id < 0 || font_id >= MINI_FONT_SLOTS || !face) return false;
    char name[48];
    if (size > 0) snprintf(name, sizeof(name), "%s-%d", face, size);
    else snprintf(name, sizeof(name), "%s", face);
    return request_name(font_id, name);
}

/* font-register!: the app's bytes for the font named in them */
static bool request_app(int id, const uint8_t *data, size_t len, bool copy) {
    return request_raw(id, data, len, copy, NULL, REQ_APP) == NULL && s_chain[id].n > 0;
}

const uint8_t *mini_font_data(int font_id) {
    if (font_id < 0 || font_id >= MINI_FONT_SLOTS || s_chain[font_id].n == 0) return NULL;
    return s_chain[font_id].src[0]->data;
}

const char *mini_font_source(int font_id) {
    if (font_id >= 0 && font_id < MINI_FONT_SLOTS && s_chain[font_id].n > 0) return s_chain[font_id].src[0]->label;
    return mini_font_builtin(font_id) ? "builtin" : NULL;
}

size_t mini_font_bytes(int font_id) {
    if (font_id >= 0 && font_id < MINI_FONT_SLOTS && s_chain[font_id].n > 0) return s_chain[font_id].src[0]->bytes;
    return 0;
}

/* ========================================================================= */
/* Font sources and replacements (config.ini [fonts])                        */
/* ========================================================================= */

static const char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = '\0';
    return s;
}

const char *mini_font_configure(const char *order, const char *overrides) {
    uint8_t new_order[SRC_COUNT];
    int n = 0;
    char buf[160];
    snprintf(buf, sizeof(buf), "%s", order && order[0] ? order : "app, fontrom, spiffs");
    for (char *p = buf; *p; ) {
        while (*p == ',' || *p == ' ' || *p == '\t') p++;
        if (!*p) break;
        char *tok = p;
        while (*p && *p != ',' && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = '\0';
        int src = -1;
        for (int i = 0; i < SRC_COUNT; i++) {
            if (strcmp(tok, s_src_names[i]) == 0) src = i;
        }
        if (strcmp(tok, "rom") == 0) src = SRC_FONTROM;
        if (strcmp(tok, "file") == 0 || strcmp(tok, "files") == 0) src = SRC_SPIFFS;
        /* "cache" (downloaded glyphs) came with the removed font server: skipped */
        if (strcmp(tok, "cache") == 0 || strcmp(tok, "server") == 0 || strcmp(tok, "net") == 0) continue;
        if (src < 0) return "font_order: sources are app, fontrom and spiffs";
        for (int i = 0; i < n; i++) {
            if (new_order[i] == src) return "font_order: a source is listed twice";
        }
        new_order[n++] = (uint8_t)src;
    }
    if (n == 0) return "font_order: no source";

    FontOverride new_over[FONT_OVERRIDES];
    int m = 0;
    char obuf[400];
    snprintf(obuf, sizeof(obuf), "%s", overrides ? overrides : "");
    char *save = obuf;
    for (char *item = save; item; item = save) {
        char *comma = strchr(item, ',');
        save = comma ? comma + 1 : NULL;
        if (comma) *comma = '\0';
        char *colon = strchr(item, ':');
        if (!colon) {
            if (trim(item)[0]) return "font_override: entries are name:replacement";
            continue;
        }
        *colon = '\0';
        const char *from = trim(item), *to = trim(colon + 1);
        if (strcmp(from, "0") == 0) from = mini_font_builtin(MINI_FONT_SYSTEM_16)->name;
        if (strcmp(from, "1") == 0) from = mini_font_builtin(MINI_FONT_CLOCK_32)->name;
        if (!from[0] || !to[0] || strlen(from) >= NAME_MAX_LEN || strlen(to) >= NAME_MAX_LEN) {
            return "font_override: names must be 1..23 characters";
        }
        if (m == FONT_OVERRIDES) return "font_override: at most 8 replacements";
        snprintf(new_over[m].from, NAME_MAX_LEN, "%s", from);
        snprintf(new_over[m].to, NAME_MAX_LEN, "%s", to);
        m++;
    }
    memcpy(s_order, new_order, sizeof(new_order));
    s_norder = n;
    memcpy(s_over, new_over, sizeof(FontOverride) * (size_t)m);
    s_nover = m;
    mini_font_rebuild();
    return NULL;
}

void mini_font_config_text(char *order, size_t order_cap, char *overrides, size_t over_cap) {
    if (order && order_cap) {
        order[0] = '\0';
        for (int i = 0; i < s_norder; i++) {
            size_t len = strlen(order);
            snprintf(order + len, order_cap - len, "%s%s", i ? ", " : "", s_src_names[s_order[i]]);
        }
    }
    if (overrides && over_cap) {
        overrides[0] = '\0';
        for (int i = 0; i < s_nover; i++) {
            size_t len = strlen(overrides);
            snprintf(overrides + len, over_cap - len, "%s%s:%s", i ? ", " : "", s_over[i].from, s_over[i].to);
        }
    }
}

/* ========================================================================= */
/* Scheme primitives                                                         */
/* ========================================================================= */

/* Every registration is also kept in the global vector %fonts (bytes, or
 * (text . kind)), so a heap snapshot (FASL) that skips the app's top level
 * gets its fonts back in mini_font_restore. */
#define FONTS_GLOBAL "%fonts"

static void font_remember(MiniVM *vm, int id, Value what) {
    mini_push_root(vm, &what);   /* first: looking up %fonts may make its symbol and collect */
    Value tab = env_lookup_name(vm, FONTS_GLOBAL);
    if (!mini_is_vector(tab) || mini_vector_length(tab) != MINI_FONT_SLOTS) {
        tab = mini_make_vector(vm, MINI_FONT_SLOTS, mini_false());
        mini_push_root(vm, &tab);
        mini_set_global(vm, FONTS_GLOBAL, tab);
        mini_pop_root(vm);
    }
    mini_vector_set(vm, tab, (size_t)id, what);
    mini_pop_root(vm);
}

/* (text . kind): kind is stream or auto for a file path, name (fontrom in
 * older snapshots) for a font requested by name */
static void font_remember_ref(MiniVM *vm, int id, const char *text, const char *kind) {
    Value p = mini_make_string(vm, text);
    mini_push_root(vm, &p);
    /* the symbol first: making it may collect, and p must be read after that (an argument
     * list evaluated the other way round passed a moved string: a crash at the next GC) */
    Value k = mini_make_symbol(vm, kind);
    Value entry = mini_make_cons(vm, p, k);
    mini_pop_root(vm);
    font_remember(vm, id, entry);
}

/* Requests the app made, resolved again (after a FASL restore, or with a
 * new font configuration). Nothing here allocates on the VM heap. */
void mini_font_restore(MiniVM *vm) {
    if (!vm) return;
    Value tab = env_lookup_name(vm, FONTS_GLOBAL);
    if (!mini_is_vector(tab) || mini_vector_length(tab) != MINI_FONT_SLOTS) return;
    for (int id = 2; id < MINI_FONT_SLOTS; id++) {
        Value v = mini_vector_ref(vm, tab, (size_t)id);
        if (mini_is_string(v) && mini_string_len(v) >= MFNT_HEADER_V1) {
            request_app(id, (const uint8_t *)mini_string_data(v), mini_string_len(v), !mini_rom_contains(vm, v));
        } else if (mini_is_pair(v) && mini_is_string(mini_car(v))) {
            const char *kind = mini_is_symbol(mini_cdr(v)) ? mini_symbol_name(mini_cdr(v)) : "auto";
            if (strcmp(kind, "name") == 0 || strcmp(kind, "fontrom") == 0) {
                request_name(id, mini_string_data(mini_car(v)));
            } else {
                mini_font_load_file(id, mini_string_data(mini_car(v)), strcmp(kind, "stream") == 0 ? 0 : MINI_FONT_RAM_LIMIT);
            }
        }
    }
}

static int font_slot_arg(MiniVM *vm, const char *who, int argc, Value *argv) {
    if (argc < 1 || !mini_is_fixnum(argv[0]) || mini_to_fixnum(argv[0]) < 2 ||
        mini_to_fixnum(argv[0]) >= MINI_FONT_SLOTS) {
        vm_error(vm, "%s: font id must be 2..%d", who, MINI_FONT_SLOTS - 1);
        return -1;
    }
    return (int)mini_to_fixnum(argv[0]);
}

/* (font-load id path ['stream]) => #t; small files go to RAM, larger ones
 * (or 'stream) are read glyph by glyph */
static Value prim_font_load(MiniVM *vm, int argc, Value *argv) {
    int id = font_slot_arg(vm, "font-load", argc, argv);
    if (id < 0) return mini_void();
    if (argc < 2 || !mini_is_string(argv[1])) {
        vm_error(vm, "font-load: expected id and file path");
        return mini_void();
    }
    bool stream = argc > 2 && mini_is_symbol(argv[2]) && strcmp(mini_symbol_name(argv[2]), "stream") == 0;
    char path[128]; /* a copy (an app's: in its folder): remembering it allocates, which may move the string */
    if (!mini_access_file(vm, "font-load", mini_string_data(argv[1]), false, path, sizeof(path))) return mini_void();
    const char *err = mini_font_load_file(id, path, stream ? 0 : MINI_FONT_RAM_LIMIT);
    if (err) {
        vm_error(vm, "font-load: %s: %s", path, err);
        return mini_void();
    }
    font_remember_ref(vm, id, path, stream ? "stream" : "auto");
    mini_gui_invalidate(0, mini_gui_get_height());
    return mini_true();
}

/* (font-register! id bytes) => #t; bytes is an MFNT font in a string, as
 * made by (include-bytes "x.mfnt"). A string of a Code ROM image is used in
 * place (no RAM); a heap string is copied. The other sources of the font's
 * name (font ROM, downloads, SPIFFS) fill the glyphs it lacks;
 * font_order / font_override decide which comes first. */
static Value prim_font_register(MiniVM *vm, int argc, Value *argv) {
    int id = font_slot_arg(vm, "font-register!", argc, argv);
    if (id < 0) return mini_void();
    if (argc < 2 || !mini_is_string(argv[1])) {
        vm_error(vm, "font-register!: expected id and font bytes (string)");
        return mini_void();
    }
    const uint8_t *data = (const uint8_t *)mini_string_data(argv[1]);
    size_t len = mini_string_len(argv[1]);
    const char *err = mini_font_validate(data, len);
    if (err) {
        vm_error(vm, "font-register!: %s", err);
        return mini_void();
    }
    bool ok = request_app(id, data, len, !mini_rom_contains(vm, argv[1]));
    font_remember(vm, id, argv[1]);
    mini_gui_invalidate(0, mini_gui_get_height());
    return mini_make_bool(ok);
}

static Value prim_font_unload(MiniVM *vm, int argc, Value *argv) {
    int id = font_slot_arg(vm, "font-unload", argc, argv);
    if (id < 0) return mini_void();
    mini_font_unload(id);
    font_remember(vm, id, mini_false());
    mini_gui_invalidate(0, mini_gui_get_height());
    return mini_void();
}

/* (font-info id) => (name height glyphs source bytes) or #f */
static Value prim_font_info(MiniVM *vm, int argc, Value *argv) {
    if (argc < 1 || !mini_is_fixnum(argv[0])) return mini_false();
    int id = (int)mini_to_fixnum(argv[0]);
    const char *src = mini_font_source(id);
    if (!src) return mini_false();
    const MiniFont *f = mini_font_get(id);
    Value list = mini_nil();
    Value tmp = mini_nil();
    mini_push_root(vm, &list);
    mini_push_root(vm, &tmp);
    list = mini_make_cons(vm, mini_make_fixnum((int64_t)mini_font_bytes(id)), list);
    tmp = mini_make_symbol(vm, src);
    list = mini_make_cons(vm, tmp, list);
    list = mini_make_cons(vm, mini_make_fixnum(f->glyph_count), list);
    list = mini_make_cons(vm, mini_make_fixnum(f->height), list);
    tmp = mini_make_string(vm, f->name ? f->name : "");
    list = mini_make_cons(vm, tmp, list);
    mini_pop_root(vm);
    mini_pop_root(vm);
    return list;
}

/* (font-sources id) => ((source name height glyphs bits) ...): the chain
 * searched for each glyph, in order */
static Value prim_font_sources(MiniVM *vm, int argc, Value *argv) {
    if (argc < 1 || !mini_is_fixnum(argv[0])) return mini_nil();
    int id = (int)mini_to_fixnum(argv[0]);
    Value list = mini_nil(), item = mini_nil(), tmp = mini_nil();
    if (id < 0 || id >= MINI_FONT_SLOTS) return list;
    mini_push_root(vm, &list);
    mini_push_root(vm, &item);
    mini_push_root(vm, &tmp);
    const Chain *c = &s_chain[id];
    for (int i = c->n - 1; i >= 0; i--) {
        const Source *s = c->src[i];
        item = mini_make_cons(vm, mini_make_fixnum(s->bpp ? s->bpp : 1), mini_nil());
        item = mini_make_cons(vm, mini_make_fixnum(s->font.glyph_count), item);
        item = mini_make_cons(vm, mini_make_fixnum(s->font.height), item);
        tmp = mini_make_string(vm, s->name);
        item = mini_make_cons(vm, tmp, item);
        tmp = mini_make_symbol(vm, s->label ? s->label : "?");
        item = mini_make_cons(vm, tmp, item);
        list = mini_make_cons(vm, item, list);
    }
    mini_pop_root(vm);
    mini_pop_root(vm);
    mini_pop_root(vm);
    return list;
}

/* (use-font id face [size]) => #t when slot id holds a font afterwards.
 * Apps built by tools/build_apps.py can carry the font (the form becomes an
 * embedded subset); otherwise the font named <face>-<size> (or face) comes
 * from the sources of font_order, glyph by glyph; with a font server the
 * glyphs nothing has are downloaded. A face with a '/' is a file path.
 * Without any source, text in that slot uses the system font. */
static Value prim_use_font(MiniVM *vm, int argc, Value *argv) {
    int id = font_slot_arg(vm, "use-font", argc, argv);
    if (id < 0) return mini_void();
    if (argc < 2 || !mini_is_string(argv[1])) {
        vm_error(vm, "use-font: expected id face [size]");
        return mini_void();
    }
    if (mini_font_source(id)) return mini_true();
    const char *face = mini_string_data(argv[1]);
    char name[48];
    if (argc > 2 && mini_is_fixnum(argv[2])) snprintf(name, sizeof(name), "%s-%d", face, (int)mini_to_fixnum(argv[2]));
    else snprintf(name, sizeof(name), "%s", face);
    if (strchr(face, '/')) {
        char path[96];
        snprintf(path, sizeof(path), "%s", face);
        if (mini_font_load_file(id, path, MINI_FONT_RAM_LIMIT)) return mini_false();
        font_remember_ref(vm, id, path, "auto");
    } else {
        bool ok = request_name(id, name);
        font_remember_ref(vm, id, name, "name");
        if (!ok) return mini_false();
    }
    mini_gui_invalidate(0, mini_gui_get_height());
    return mini_true();
}

/* (font-config) => ((app fontrom spiffs) (("system_16" . "simsun-16") ...))
 * (font-config order [overrides]): sets font_order and font_override as in
 * config.ini (strings) and resolves every font again */
static Value prim_font_config(MiniVM *vm, int argc, Value *argv) {
    char order[160], over[400];
    mini_font_config_text(order, sizeof(order), over, sizeof(over));
    if (argc >= 1) {
        if (!mini_is_string(argv[0]) || (argc >= 2 && !mini_is_string(argv[1]))) {
            vm_error(vm, "font-config: expected order string [overrides string]");
            return mini_void();
        }
        snprintf(order, sizeof(order), "%s", mini_string_data(argv[0]));
        if (argc >= 2) snprintf(over, sizeof(over), "%s", mini_string_data(argv[1]));
        const char *err = mini_font_configure(order, over);
        if (err) {
            vm_error(vm, "font-config: %s", err);
            return mini_void();
        }
        mini_font_restore(vm);
        mini_gui_invalidate(0, mini_gui_get_height());
    }
    Value srcs = mini_nil(), pairs = mini_nil(), a = mini_nil(), b = mini_nil();
    mini_push_root(vm, &srcs);
    mini_push_root(vm, &pairs);
    mini_push_root(vm, &a);
    mini_push_root(vm, &b);
    for (int i = s_norder - 1; i >= 0; i--) {
        a = mini_make_symbol(vm, s_src_names[s_order[i]]);
        srcs = mini_make_cons(vm, a, srcs);
    }
    for (int i = s_nover - 1; i >= 0; i--) {
        a = mini_make_string(vm, s_over[i].from);
        b = mini_make_string(vm, s_over[i].to);
        a = mini_make_cons(vm, a, b);
        pairs = mini_make_cons(vm, a, pairs);
    }
    a = mini_make_cons(vm, pairs, mini_nil());
    a = mini_make_cons(vm, srcs, a);
    mini_pop_root(vm);
    mini_pop_root(vm);
    mini_pop_root(vm);
    mini_pop_root(vm);
    return a;
}

void mini_register_font_lib(MiniVM *vm) {
    static const MiniPrimEntry funcs[] = {
        { "font-config",    prim_font_config },
        { "font-load",      prim_font_load },
        { "font-register!", prim_font_register },
        { "font-unload",    prim_font_unload },
        { "font-info",      prim_font_info },
        { "font-sources",   prim_font_sources },
        { "use-font",       prim_use_font },
    };
    mini_register_prim_table(vm, funcs, sizeof(funcs) / sizeof(funcs[0]));
    mini_register_fontrom_lib(vm);
}

/* The font registry's heap blocks, for the heap map */
void mini_font_mem_owners(MiniMemOwnerFn fn, void *user) {
    for (int i = 0; i < SOURCE_POOL; i++) if (s_pool[i]) fn(s_pool[i], "font source", user);
    for (int i = 0; i < MINI_FONT_SLOTS; i++) {
        fn(s_req[i].owned, "font copy", user);
        fn(s_req[i].path, "font path", user);
    }
    fn(s_gc_buf, "glyph cache", user);
    fn(s_gc, "glyph cache index", user);
}
