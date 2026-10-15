/**
 * @file minischeme_fontrom.h
 * @brief System font ROM: a flash partition of MFNT fonts read in place.
 *
 * The partition is memory-mapped (XIP on the ESP32), so a font in it costs no
 * RAM and every glyph is a plain memory read. Fonts are added and removed one
 * by one (fontrom-add / fontrom-remove, the WebREPL font manager) or the whole
 * image is written by tools/make_fontrom.py. (use-font id face size) finds
 * fonts here by name "<face>-<size>" when the app does not carry its own.
 *
 * Layout (little-endian, 4 KB sectors):
 *   sectors 0 and 1  two copies of the directory; the valid one with the
 *                    higher sequence number is current, the other is the
 *                    next one written (a power loss keeps the old directory)
 *                    0 "MFRM", 4 u16 version 1, 6 u16 count, 8 u32 sequence,
 *                    12 u32 FNV-1a of bytes 4..11 and the entries,
 *                    16 up to 32 entries of 32 bytes: char name[24], u32 offset,
 *                    u32 length
 *   sector 2..       fonts, each starting on a sector boundary, packed in
 *                    directory order (removing a font moves later ones down)
 */

#ifndef MINISCHEME_FONTROM_H
#define MINISCHEME_FONTROM_H

#include "minischeme.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MINI_FONTROM_SECTOR 4096u
#define MINI_FONTROM_NAME   24      /* name bytes including the NUL */

/** @brief Flash backend: a mapped partition plus erase and write. */
typedef struct {
    const uint8_t *map;   /* the whole partition, readable in place */
    size_t size;          /* bytes, a multiple of MINI_FONTROM_SECTOR */
    bool (*erase)(size_t offset, size_t len, void *user);  /* sector aligned */
    bool (*write)(size_t offset, const void *data, size_t len, void *user);
    void *user;
} MiniFontRomFlash;

/** @brief Uses the partition (NULL detaches); bad entries are skipped. */
void mini_fontrom_attach(const MiniFontRomFlash *flash);
bool mini_fontrom_present(void);

/** @brief Font bytes by name, in place; NULL when absent. */
const uint8_t *mini_fontrom_find(const char *name, size_t *len);
int mini_fontrom_count(void);
/** @brief Name of font i (0..count-1), its bytes through data/len. */
const char *mini_fontrom_entry(int i, const uint8_t **data, size_t *len);
size_t mini_fontrom_used(void);
size_t mini_fontrom_capacity(void);

/* Adding a font: begin with its total length, write it in pieces, end. A
 * NULL or empty name takes the name stored in the font. A font with the same
 * name is replaced. Font slots using the partition follow moved fonts. All
 * return NULL on success, else an error message. */
const char *mini_fontrom_add_begin(const char *name, size_t len);
const char *mini_fontrom_add_write(const void *data, size_t len);
const char *mini_fontrom_add_end(void);
void mini_fontrom_add_abort(void);
/** @brief Adds an MFNT file (read in pieces); name as for add_begin. */
const char *mini_fontrom_add_file(const char *path, const char *name);
const char *mini_fontrom_remove(const char *name);

/** @brief fontrom-list, fontrom-info, fontrom-add, fontrom-remove. */
void mini_register_fontrom_lib(MiniVM *vm);

#endif /* MINISCHEME_FONTROM_H */
