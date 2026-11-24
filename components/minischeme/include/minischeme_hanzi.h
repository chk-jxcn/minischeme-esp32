/**
 * @file minischeme_hanzi.h
 * @brief Simplified / traditional Chinese on screen: every character drawn is converted to
 *        the chosen script (config.ini [fonts] font_chinese).
 *
 * The conversion is character by character (tables in minischeme_hanzi_table.h): a character
 * with several counterparts takes one of them. Strings themselves never change; only what the
 * text layout decodes (mini_utf8_next) does, so drawing, widths and wrapping agree.
 */

#ifndef MINISCHEME_HANZI_H
#define MINISCHEME_HANZI_H

#include <stdint.h>

#define MINI_HANZI_KEEP         0   /* as written */
#define MINI_HANZI_SIMPLIFIED   1   /* traditional characters drawn simplified */
#define MINI_HANZI_TRADITIONAL  2   /* simplified characters drawn traditional */

/** @brief The script text is drawn in from now on. */
void mini_hanzi_set_mode(int mode);
/** @brief The current script (MINI_HANZI_...). */
int mini_hanzi_mode(void);
/** @brief "keep", "simplified" or "traditional" (also 简体 / 繁体, s / t) -> MINI_HANZI_...; -1 otherwise. */
int mini_hanzi_parse(const char *value);
/** @brief The code point to draw for cp in the current script. */
uint32_t mini_hanzi_map(uint32_t cp);
/** @brief The code point of cp in the given script (any mode). */
uint32_t mini_hanzi_convert(uint32_t cp, int mode);

#endif
