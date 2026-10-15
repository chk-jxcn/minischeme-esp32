/**
 * @file minischeme_mirror.h
 * @brief Screen mirroring: the rows the device sent to its panel, captured
 *        again and compressed for a remote viewer (WebREPL /api/screen).
 *
 * The host marks rows as it hands them to the panel; a viewer request
 * re-renders just those rows (mini_gui_capture, no panel traffic) and packs
 * them, so a mirror costs nothing while the screen is still and no frame
 * buffer is needed in BAND mode.
 *
 * Every row keeps a version: the value of a clock that each mark bumps. A
 * viewer asks for the rows newer than its own cursor (since), so any number
 * of viewers each get every change; nothing is cleared by a reply. A pass
 * over the screen may take several replies (from: the row to go on at);
 * when it ends, the viewer's cursor becomes the clock of the pass's first
 * reply, so rows changed meanwhile come in the next pass.
 *
 * Response (little-endian):
 *   0  "MSCR", u8 version 2, u8 flags (1: more rows pending, 2: this mode
 *      keeps no pixels: DIRECT), u16 width, u16 height, u16 row count,
 *      u32 clock (the version of the newest change; version 1, from old
 *      firmware, put a reply counter here)
 *   16 rows: u16 y, u16 bytes, PackBits16 data of `width` RGB565 pixels:
 *      control c < 128: c + 1 literal pixels follow; c >= 128: the next
 *      pixel repeated c - 126 times (2..129)
 */

#ifndef MINISCHEME_MIRROR_H
#define MINISCHEME_MIRROR_H

#include "minischeme.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MINI_MIRROR_HEADER   16
#define MINI_MIRROR_MAX_ROWS 512
#define MINI_MIRROR_MORE     1
#define MINI_MIRROR_NOPIXELS 2

/** @brief Rows [y, y + h) changed on the panel. */
void mini_mirror_mark(int y, int h);
/** @brief Every row changed. */
void mini_mirror_mark_all(void);
/** @brief The clock: the version of the newest change (1 before any). */
uint32_t mini_mirror_clock(void);
/** @brief Fills out with the rows newer than since, from row `from` down,
 *         as many as fit in cap (at least one full row: 16 + 4 + width * 2
 *         + width / 128 + 1 bytes); MORE when some were left for the next
 *         request (from = the last row sent + 1). since 0, or one ahead of
 *         the clock (a viewer from before a restart): the whole screen.
 *         Returns the bytes used. */
size_t mini_mirror_fill_since(MiniVM *vm, uint8_t *out, size_t cap, uint32_t since, int from);
/** @brief The same for a viewer that keeps no cursor (an old WebREPL): one
 *         shared cursor, so two such viewers split the changes between them. */
size_t mini_mirror_fill(MiniVM *vm, uint8_t *out, size_t cap);
/** @brief That shared cursor starts over: the next fills send the whole screen. */
void mini_mirror_restart(void);
/** @brief The row version table, for the heap map. */
void mini_mirror_mem_owners(void (*fn)(const void *block, const char *what, void *user), void *user);
/** @brief Worst-case PackBits16 size of one row of width pixels. */
size_t mini_mirror_row_max(int width);
/** @brief Packs count RGB565 pixels; returns the bytes written. */
size_t mini_mirror_pack(const uint16_t *px, int count, uint8_t *out);

#endif /* MINISCHEME_MIRROR_H */
