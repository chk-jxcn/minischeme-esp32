/**
 * @file minischeme_device.h
 * @brief Device application runtime shared by the ESP32 firmware and the
 *        WASM/PC simulators: wall clock formatting, lunar calendar, and the
 *        1 Hz app tick. Keeping this in one place guarantees that the
 *        simulator feeds the Scheme app exactly what the hardware does.
 */

#ifndef MINISCHEME_DEVICE_H
#define MINISCHEME_DEVICE_H

#include "minischeme.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool synced;   /* false until NTP (or the host clock) provides real time */
    int  year;     /* e.g. 2026 */
    int  month;    /* 1..12 */
    int  day;      /* 1..31 */
    int  hour;     /* 0..23 */
    int  minute;   /* 0..59 */
    int  second;   /* 0..59 */
    int  wday;     /* 0 = Sunday .. 6 */
} MiniWallTime;

/* Placeholders shown until the clock is synchronized (no fake time) */
#define MINI_CLOCK_PLACEHOLDER_HM   "--:--"
#define MINI_CLOCK_PLACEHOLDER_SEC  ":--"
#define MINI_CLOCK_PLACEHOLDER_WEEK "未同步"
#define MINI_CLOCK_PLACEHOLDER_DATE "等待网络校时"

/* Converts a Gregorian date (1900-01-31 .. 2100-12-31) to the Chinese lunar
 * calendar. Returns false when the date is out of range. */
bool mini_lunar_from_solar(int year, int month, int day,
                           int *out_lmonth, int *out_lday, bool *out_leap);

/* Writes the LCD display text (Chinese lunar month/day) into buf.
 * Returns false when out of range. */
bool mini_lunar_format(int year, int month, int day, char *buf, size_t len);

/* Calls (app-tick hm sec week date lux bl tick lunar) on the VM.
 * `tick` is a monotonic seconds counter that drives sensor cadence even while
 * the wall clock is unsynchronized. */
void mini_device_clock_tick(MiniVM *vm, const MiniWallTime *t, int tick, int lux, int backlight);

/* Calls the running app's hook (e.g. "app-tick") when it defines one, else the
 * procedure named `fallback` (NULL: nothing). Arguments must be rooted. */
Value mini_device_call_hook(MiniVM *vm, const char *hook, const char *fallback, int argc, Value *argv);
/** @brief True when the app defines the hook (a procedure bound to that name). */
bool mini_device_has_hook(MiniVM *vm, const char *hook);

/* After an app is restored from a FASL snapshot: calls (app-resume) when the
 * app defines it. A snapshot holds Scheme state only, so the app restarts its
 * C-side services there (JSON feeds and the like). */
void mini_device_resume_app(MiniVM *vm);

/* Button gestures, shared by the firmware (GPIO 18 and the WebREPL's remote
 * button) and the WASM device. Feed the button state on every main-loop
 * pass (and on every change). Holding only arms a gesture; it runs on
 * release. The short, the long and the mid press belong to the app:
 *   released before the long time             -> (app-button), MINI_BUTTON_SHORT
 *   released after the long time (0.8 s)      -> (app-long-press), MINI_BUTTON_LONG
 *   released after the mid time (3.5 s)       -> (app-mid-press), MINI_BUTTON_MID
 *   released after the hold time (8 s)        -> the hardware pages open or close
 *                                                ((info-long-press), apps/lib/infopages.scm),
 *                                                MINI_BUTTON_INFO
 * While the hardware pages show they take the presses: short turns them, long
 * closes them, mid returns MINI_BUTTON_SYSTEM and the host opens the system
 * settings. The times come from config.ini [system] button_long_ms,
 * button_mid_ms, button_hold_ms (mini_device_button_config). While held, up
 * to three bars in the bottom-right corner show the progress
 * (mini_gui_set_hold_overlay): the long press bar from 5/8 of the long time,
 * the mid press bar above it from the long time, the hold bar on top from
 * the mid time; pale green while filling, red once armed. Inside the system
 * settings there is no hold: short, long and mid press (holding longer is a
 * mid press) go to the settings app (next, choose, back).
 * A release counts once it has lasted MINI_BUTTON_DEBOUNCE_MS, so contact
 * bounce never makes two presses, while a remote tap of one pass still does. */
#define MINI_BUTTON_LONG_MS       800     /* defaults of the three times */
#define MINI_BUTTON_MID_MS        3500
#define MINI_BUTTON_HOLD_MS       8000
#define MINI_BUTTON_DEBOUNCE_MS   30
typedef enum { MINI_BUTTON_NONE, MINI_BUTTON_SHORT, MINI_BUTTON_LONG, MINI_BUTTON_SYSTEM, MINI_BUTTON_INFO,
               MINI_BUTTON_MID } MiniButtonEvent;
MiniButtonEvent mini_device_button_update(MiniVM *vm, bool pressed, uint32_t now_ms);
/* Whether the hold (the hardware pages, and from them the system settings)
 * exists: off inside the system settings, where holding longer is still a
 * mid press */
void mini_device_button_system_gesture(bool on);
/* The three times in ms; 0 keeps the default. Each is kept in its range:
 * long 300..3000, mid at least long + 500 and at most 15000, hold at least
 * mid + 1000 and at most 30000. */
void mini_device_button_timing(uint32_t long_ms, uint32_t mid_ms, uint32_t hold_ms);
/* The times from config.ini text: [system] button_long_ms, button_mid_ms,
 * button_hold_ms (missing or empty: the defaults) */
void mini_device_button_config(const char *config_text);
void mini_device_button_get_timing(uint32_t *long_ms, uint32_t *mid_ms, uint32_t *hold_ms);
/* Held time of the current press in ms (0 when released), for hold feedback */
uint32_t mini_device_button_held_ms(uint32_t now_ms);
/* MINI_BUTTON_SYSTEM (a mid press on the hardware pages): the host restarts
 * into the system settings app (examples/app_settings_msbc.h) */

/* App metadata, appended to an app image (.msbc / .fasl) by
 * tools/build_apps.py; readers of the image stop before it:
 *   [image][meta text][u32 meta length][u32 FNV-1a of the meta]["MSMD"]
 * The meta text is INI: [app] id, name, description, version, pages, and a
 * [config] section that is the app's default <id>.ini (comments included). */
#define MINI_APP_META_FOOTER 12
#define MINI_APP_META_MAX    8192
/* The meta text inside an image's bytes (not NUL-terminated), or NULL */
const char *mini_app_meta_find(const uint8_t *data, size_t len, size_t *meta_len);
/* The meta text of an image file, malloc'd and NUL-terminated, or NULL */
char *mini_app_meta_read_file(const char *path);
/* The [config] section of a meta text (from its "[config]" line), or NULL */
const char *mini_app_meta_config(const char *meta);
/* The lines of `defaults` (an app's [config]) whose keys the app's existing
 * <id>.ini text does not set (an empty value counts as set), so settings
 * added by an app update reach it. malloc'd, "" when none; NULL: no memory */
char *mini_app_ini_missing(const char *ini, const char *defaults);
/* The id of an app: its meta's [app] id, else the file name without its
 * extension ("/spiffs/apps/weather.msbc" -> "weather") */
void mini_app_id(const char *path, const char *meta, char *out, size_t cap);
/* One entry of (app-list): (path id name description pages version), with
 * the file name standing in for a missing name */
Value mini_app_entry(MiniVM *vm, const char *path, const char *meta);
/* One app as JSON for the settings page and the WebREPL's app manager:
 * {"key","source","path","dir","id","name","description","version","ini",
 *  "pages","size","boot","running","web"[,"meta"]} where meta (with
 * MINI_APP_META) is the whole metadata text ([app], [fields], [config]).
 * Returns the length, or 0 when it does not fit in cap. */
#define MINI_APP_BOOT    1
#define MINI_APP_RUNNING 2
#define MINI_APP_META    4
#define MINI_APP_WEB     8
size_t mini_app_json(char *out, size_t cap, const char *path, const char *meta, long size, int flags);
/* FNV-1a 32-bit, as the trailer's check */
uint32_t mini_fnv1a(const uint8_t *data, size_t len);

/* ---- App folders ----------------------------------------------------------
 * Every app has a folder: its image (app.msbc or app.fasl), its settings
 * (config.ini) and its data.
 *   /spiffs/apps/<id>/   installed from the market (the settings too)
 *   /spiffs/user/<id>/   uploaded by the user, not managed by the market;
 *                        the same id may be installed both ways
 * The built-in app (/spiffs/app.msbc) keeps its data in /spiffs/apps/<id>/. */
#define MINI_APPS_DIR "/spiffs/apps"
#define MINI_USER_DIR "/spiffs/user"
/* An app id: 1..31 of [A-Za-z0-9_-] (a folder name, nothing else) */
bool mini_app_id_ok(const char *id);
/* The folder of the app whose image is path ("/spiffs/apps/weather") */
void mini_app_dir_of(const char *path, const char *id, char *out, size_t cap);
/* "market", "user" or "builtin" by where the image is */
const char *mini_app_source(const char *path);
/* The app's key in the web API: "apps/<id>", "user/<id>" or "builtin" */
void mini_app_key(const char *path, char *out, size_t cap);
/* The folder of a key ("apps/<id>" -> "/spiffs/apps/<id>"); false for any
 * other text (and for "builtin", which has no folder of its own) */
bool mini_app_key_dir(const char *key, char *out, size_t cap);
/* The new name, under /spiffs/apps, of a file of the old flat layout:
 * "weather.msbc" -> "weather/app.msbc", "weather.ini" -> "weather/config.ini",
 * "memo.txt" -> "memo/memo.txt". false: not an app's file. */
bool mini_app_legacy_name(const char *name, char *out, size_t cap);
/* An old boot_app value "apps/weather.msbc" -> "apps/weather/app.msbc";
 * false when it needs no change */
bool mini_app_legacy_boot(const char *value, char *out, size_t cap);

/* ---- The app sandbox --------------------------------------------------------
 * While an app runs, its own code may touch only its folder: relative paths
 * are in it, absolute ones must be; its image there is read-only; its files
 * together may take at most the storage quota. Privileged procedures
 * (mini_access_privileged: config-set!, Wi-Fi, the font ROM, restart, ...)
 * and config.ini secrets (Wi-Fi, keys named *password*, *secret*, *token*)
 * are not for it. The system settings run without a sandbox; code from the
 * REPL (WebREPL, serial) runs privileged between mini_app_privileged_begin
 * and _end. */
/* The running app's folder, "" for none (the system settings) */
void mini_app_sandbox_set(const char *dir);
const char *mini_app_sandbox_dir(void);
/* true while the app's own code runs (not privileged code) */
bool mini_app_sandboxed(void);
void mini_app_privileged_begin(void);
void mini_app_privileged_end(void);
/* Storage quota of an app folder and how the host measures one */
void mini_app_set_storage(size_t quota, size_t (*dir_used)(const char *dir));
/* Whether writing `bytes` to a file of `old` bytes fits the quota; why if not */
bool mini_app_storage_ok(size_t bytes, size_t old, char *why, size_t cap);
/** @brief Bytes left in the running app's folder quota ((size_t)-1 without a sandbox). */
size_t mini_app_storage_room(void);
/* Whether (config-get key) may fall back to config.ini for the running code */
bool mini_app_config_key_ok(const char *key);

/* An app's own web page (apps/<id>/web.html, gzip'd by tools/build_apps.py),
 * right before the meta text:
 *   [image][page.gz][u32 length][u32 FNV-1a of page.gz]["MSWP"][meta trailer]
 * The board serves it at /app; its calls go to the app's (app-web name body). */
#define MINI_APP_WEB_FOOTER 12
/* The gzip'd page inside an image's bytes (checked), or NULL */
const uint8_t *mini_app_web_find(const uint8_t *data, size_t len, size_t *web_len);
/* Where the page is in an image file: false when it has none */
bool mini_app_web_locate_file(const char *path, long *offset, size_t *web_len);

/* The config.ini the firmware writes on first boot; the WASM device seeds its
 * simulated SPIFFS with the same file */
extern const char mini_device_config_template[];

/* Looks up `key` (case-insensitive) in config.ini text; ';' and '#' start
 * comments. Copies the trimmed value into out and returns true when found. */
bool mini_device_config_value(const char *text, const char *key, char *out, size_t cap);

/* config.ini text with `key = value`: the value of the key's line is
 * replaced, its comment kept in place; a missing key is added at the end of
 * [section] (a missing section at the end of the text). Returns malloc'd
 * text, or NULL when out of memory. */
char *mini_device_config_set(const char *text, const char *section, const char *key, const char *value);

/* [fonts] font_order / font_override of config.ini text (missing keys:
 * defaults) applied to the font registry; NULL or an error message.
 * mini_device_config_fonts_section is appended to older files that lack it. */
const char *mini_device_apply_font_config(const char *ini);
/* Only its font server part (font_server, font_server_on, _first, _face):
 * the settings page applies it without a restart */
void mini_device_apply_fontnet_config(const char *ini);
extern const char mini_device_config_fonts_section[];
/* config.ini [text]: marquee / marquee_fps / marquee_step for one-line text
 * and wrap_marquee / wrap_marquee_fps / wrap_marquee_step for wrapped text
 * (defaults when missing); the section is appended to older files */
extern const char mini_device_config_text_section[];
void mini_device_apply_text_config(const char *ini);

/* A progress bar (track and fill) across the middle of a black screen,
 * drawn from C (no VM needed), e.g. while an app's Code ROM image is built
 * at boot. The first call clears the screen; later ones redraw only the
 * bar's rows, at most every 2% and 80 ms (permille 1000 always). */
void mini_device_progress(int permille, uint32_t now_ms);

/* App failures. mini_device_app_error notes an error of a hook or a render
 * ("render") and says whether the app has crashed: 10 errors within 10 s,
 * 3 failed renders in a row, or out of memory; a good render resets the
 * render count. The host then shows mini_device_error_screen and restarts
 * into the system settings. Phases: "rom" (image build), "load", "crash". */
bool mini_device_app_error(const char *where, const char *err, uint32_t now_ms);
void mini_device_app_render_ok(void);
void mini_device_app_health_reset(void);
const char *mini_device_app_crash_reason(void);   /* "where: error" of the last one */
const char *mini_device_fail_title(const char *phase);
void mini_device_error_screen(const char *title, const char *name, const char *reason);
void mini_device_progress_end(void);
int  mini_device_progress_value(void);   /* -1 when not shown */
/* Appended when config.ini lacks [system] boot_app (with [wifi] and the app proxy) */
extern const char mini_device_config_system_section[];

/* Registers (lunar-date y m d) => (month day leap?), (lunar-date-string y m d)
 * and (system-info) => alist of chip, memory, flash, network, time, display,
 * peripherals */
void mini_register_device_lib(MiniVM *vm);

#ifdef __cplusplus
}
#endif

#endif /* MINISCHEME_DEVICE_H */
