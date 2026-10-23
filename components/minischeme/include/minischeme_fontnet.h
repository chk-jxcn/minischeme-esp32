/**
 * @file minischeme_fontnet.h
 * @brief Glyphs from the font server (minischeme-proxy font_server.py).
 *
 * The board asks a font server, without a password, for the glyphs of its
 * system font face and height (config.ini [fonts]):
 *   font_server        the server's URL
 *   font_server_on     0: never ask it
 *   font_server_first  0: the board's fonts first, the server fills gaps
 *                      (characters they lack, emoji); 1: the server's face
 *                      first for system-font text, the board's fonts fill
 *                      gaps and stand in until a glyph has arrived
 *   font_server_face   the face asked for (empty: the system font's face)
 * Glyphs are kept in a file of hashed slots (/spiffs/fonts/net-<face>-<h>.mgh),
 * the recently used ones in a small RAM cache; a character no font has yet
 * is asked for (a batch per request, one request at a time) and the screen
 * is redrawn when it arrives.
 *
 * Transport: the host takes a request URL (mini_fontnet_take), fetches it and
 * hands the reply back (mini_fontnet_deliver), both on the main thread. On
 * the ESP32 the feed's task fetches (mini_fontnet_poll in the main loop);
 * the WebREPL page fetches for the WASM device.
 */

#ifndef MINISCHEME_FONTNET_H
#define MINISCHEME_FONTNET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "minischeme_fonts.h"

#define MINI_FONTNET_DEFAULT_URL "http://124.222.231.186:8090"

/* config.ini [fonts]; url NULL: the default; "" turns it off too */
void mini_fontnet_configure(const char *url, bool on, bool first, const char *face);
bool mini_fontnet_first(void);
/* For the settings page: "off", "idle", "queued", "fetching" or "waiting"
 * (after a failure), the glyphs received and absent since boot, the last
 * error ("" when none) */
const char *mini_fontnet_status(uint32_t *received, uint32_t *absent, const char **error);

/* The server's glyph of cp for system-font text, NULL when there is none
 * (yet): the store does not know cp, which is then asked for, or the server
 * has none. *used: a font of the system font's height. */
const MiniGlyph *mini_fontnet_glyph(uint32_t cp, const MiniFont **used);
/* Its advance, -1 when unknown or absent */
int mini_fontnet_advance(uint32_t cp);

/* The URL of the next request (now in flight), NULL when none is due */
const char *mini_fontnet_take(void);
/* Its outcome: status 200 and the reply body, else the HTTP status (0: no
 * connection) and an error text */
void mini_fontnet_deliver(const uint8_t *body, size_t len, int status, const char *error);
/* ESP32 main loop: hands a due request to the feed's task, delivers a
 * finished one (no-op elsewhere) */
void mini_fontnet_poll(void);

/* Storage of the glyph file, stdio by default; the WebREPL sets its own */
typedef struct {
    long (*size)(const char *path);                                     /* -1: no file */
    bool (*read)(const char *path, long off, void *buf, size_t n);
    bool (*write)(const char *path, long off, const void *buf, size_t n); /* creates, extends */
    void (*remove)(const char *path);
} MiniFontnetIO;
void mini_fontnet_set_io(const MiniFontnetIO *io);

/* Code points drawn as nothing when no font has them (variation selectors,
 * zero-width joiners, ...): never asked for, no box */
bool mini_font_ignorable(uint32_t cp);

struct MiniVM;
void mini_register_fontnet_lib(struct MiniVM *vm);

#endif /* MINISCHEME_FONTNET_H */
