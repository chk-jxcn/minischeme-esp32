/**
 * @file minischeme_fonts.h
 * @brief Dot-matrix font tables (generated) and the font registry API.
 *
 * Fonts 0 and 1 are compiled into the firmware (minischeme_fonts.c, written
 * by tools/compile_font_to_fasl.py). Every slot 0..7 can hold a chain of
 * sources searched glyph by glyph (minischeme_fontreg.c): the font an app
 * carries, the system font ROM and MFNT files on SPIFFS; the built-in 16 px
 * font is the last fallback.
 */

#ifndef MINISCHEME_FONTS_H
#define MINISCHEME_FONTS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define MINI_FONT_SYSTEM_16 0
#define MINI_FONT_CLOCK_32  1
#define MINI_FONT_SLOTS     8   /* ids 0..7; 2..7 can be registered or loaded */

typedef struct {
    uint32_t codepoint;        /* Unicode scalar value (emoji are above 0xFFFF) */
    uint8_t  advance;
    uint8_t  bytes_per_row;
    const uint8_t *bitmap;
    uint8_t  bpp;              /* 0 or 1: one bit per pixel; 2 or 4: coverage (anti-aliased) */
} MiniGlyph;

typedef struct {
    int id;
    const char *name;
    uint8_t height;
    int glyph_count;
    const MiniGlyph *glyphs;   /* NULL for MFNT fonts (looked up in the registry) */
    uint8_t ascent;            /* baseline row from the top, 0 when unknown */
} MiniFont;

/* Built-in fonts (generated tables) */
const MiniFont *mini_font_builtin(int font_id);

/* Any font id: the slot's font (0 and 1: a replacement of the built-in font),
 * else the built-in font; unknown ids fall back to the system font */
const MiniFont *mini_font_get(int font_id);
const MiniGlyph *mini_font_find_glyph(const MiniFont *font, uint32_t codepoint);
int mini_font_text_width(int font_id, const char *utf8_str);

/* MFNT fonts (version 1, or 2 with coverage bits and a baseline). register:
 * data stays where it is (Flash, a string literal of a Code ROM image)
 * unless copy is true. load: small files are read into RAM, larger ones are
 * read glyph by glyph through a cache. The slot then holds just that font.
 * Both return NULL on success, else an error message. */
const char *mini_font_register(int font_id, const uint8_t *data, size_t len, bool copy);
const char *mini_font_load_file(int font_id, const char *path, size_t ram_limit);
void mini_font_unload(int font_id);
/* use-font from C: slot font_id (0..7) resolved from the font named
 * <face>-<size> through every configured source; true when one has it */
bool mini_font_use(int font_id, const char *face, int size);
/* Same with the source label font-info reports (NULL: "rom" or "ram") */
const char *mini_font_register_ex(int font_id, const uint8_t *data, size_t len, bool copy, const char *source);
/* Full check of an MFNT image: header, sorted index, bitmaps inside */
const char *mini_font_validate(const uint8_t *data, size_t len);
/* Source of the slot's first font: "builtin", "rom", "ram", "file",
 * "fontrom" or NULL for an empty slot */
const char *mini_font_source(int font_id);
size_t mini_font_bytes(int font_id);
/* In-memory bytes of the slot's first font; NULL for other kinds */
const uint8_t *mini_font_data(int font_id);
/* The glyph of codepoint for text in font: the font (every source of its
 * chain), else the system font (font 0, maybe replaced), else the built-in
 * 16 px font; *used: the font it came from (its height and baseline) */
const MiniGlyph *mini_font_glyph(const MiniFont *font, uint32_t codepoint, const MiniFont **used);
/* Advance used for layout (missing glyphs as the text drawing falls back) */
int mini_font_advance(const MiniFont *font, uint32_t codepoint);
/* The next code point of a UTF-8 string, advancing *pp past it (1 to 4
 * bytes; a broken sequence gives its first byte) */
uint32_t mini_utf8_next(const unsigned char **pp);
/* The system font's face and height ("simsun", 16 for simsun-16; "system"
 * for the built-in font): what the font server is asked for */
const char *mini_font_system_face(int *height);
/* Where fonts come from (config.ini [fonts]): order lists app, fontrom,
 * cache (glyphs from the font server) and spiffs (NULL/empty: "app, fontrom,
 * cache, spiffs"); overrides is "name:other, ..." where name may be
 * system_16 (or 0) and clock_32 (or 1) for the built-in fonts. Rebuilds
 * every font; NULL or an error message. */
const char *mini_font_configure(const char *order, const char *overrides);
void mini_font_config_text(char *order, size_t order_cap, char *overrides, size_t over_cap);
/* Rebuilds every slot from the request that filled it (the font ROM, the
 * cache or the configuration changed) */
void mini_font_rebuild(void);
/* Drops every source reading [lo, hi) (the font ROM is about to change) */
void mini_font_release_range(const uint8_t *lo, const uint8_t *hi);
/* Optional reader tried when fopen fails (WebREPL mock file system): returns
 * the file's bytes, or NULL; the font is copied */
typedef const uint8_t *(*MiniFontFileHook)(const char *path, size_t *len);
void mini_font_set_file_hook(MiniFontFileHook hook);
const uint8_t *mini_font_hook_read(const char *path, size_t *len);

#endif /* MINISCHEME_FONTS_H */
