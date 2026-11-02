/**
 * @file minischeme_market.h
 * @brief The app market: install and update apps from a market server.
 *
 * A market is a folder served over plain HTTP, laid out like a package
 * repository (tools/build_apps.py writes it): index.json, an array of
 * {"id","name","description","version","api","extensions","file","size","hash"}
 * for browsers (the device page searches and compares it itself), one
 * <id>.info.json per app holding that app's entry, and the app images. The
 * app proxy (minischeme-proxy) serves its apps/ as /market/ (and makes an
 * <id>.info.json from index.json when the file is missing); config.ini
 * [market] market_url points elsewhere.
 *
 * The device never reads index.json: a market of any size costs it nothing.
 * The engine (shared by the firmware and the WASM simulator) lists the
 * apps installed here (host installed()), fetches each one's <id>.info.json
 * in turn, compares it with the installed copy, and installs: an app not
 * installed yet is asked for by id (its info first). An image is
 * downloaded to a temporary file, checked (size, FNV-1a hash, image magic,
 * API compatibility) and only then replaces the installed file, so a failed
 * download never breaks an app. A running app is not disturbed: it runs from
 * its Code ROM image, rebuilt from the new file at the next boot.
 *
 * While an app runs the market only reads the index (what is installed, what
 * has an update); installs and updates run in the system settings.
 *
 * Automatic updates (config.ini [market] market_auto_update = 1): an app's
 * boot checks the running app alone (its <id>.info.json, a few hundred
 * bytes), once, when the network comes up within the boot window; with an
 * update the host restarts into the system settings, which check every
 * installed app, install the updates and then return to the app (status
 * "return"). No network at boot, or a failed check: skipped until the next
 * boot, never retried. mini_market_update_all starts the same round from
 * the device page (全部更新).
 *
 * The host fetches URLs (a background HTTP task on the board, the page on
 * the simulator) and does the file work; all engine state changes happen in
 * mini_market_poll() and the calls below, from the host's main loop.
 */

#ifndef MINISCHEME_MARKET_H
#define MINISCHEME_MARKET_H

#include "minischeme.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MINI_MARKET_ID_MAX 24      /* an app id and its NUL */
#define MINI_MARKET_MAX_LOCAL 48   /* installed apps one round checks */

typedef struct MiniMarketHost {
    /* Start fetching url: into memory (file NULL, at most max bytes) or into
     * the file `file` (replaced). false when it cannot start. */
    bool (*fetch_start)(const char *url, const char *file, size_t max);
    /* 0 running, 1 done, -1 failed (*err set); bytes so far and expected (0: unknown) */
    int (*fetch_poll)(size_t *received, size_t *total, const char **err);
    /* The memory result (until fetch_release) */
    const char *(*fetch_data)(size_t *len);
    void (*fetch_release)(void);
    void (*fetch_cancel)(void);
    /* Size, FNV-1a hash and first 4 bytes of a file; false when missing */
    bool (*file_hash)(const char *path, size_t *size, uint32_t *hash, uint8_t head[4]);
    /* The metadata text of an app image (malloc'd), NULL when it has none */
    char *(*file_meta)(const char *path);
    /* Move tmp over path (atomically where the file system can) */
    bool (*file_replace)(const char *tmp, const char *path);
    void (*file_remove)(const char *path);
    /* A config.ini value ("" when unset): market_url, proxy */
    bool (*config)(const char *key, char *out, size_t cap);
    /* Whether the network is up */
    bool (*online)(void);
    void (*log)(const char *msg);
    /* Whether installs may run now: on the board only in the system
     * settings (an app's VM leaves too little memory); NULL: always */
    bool (*install_here)(void);
    /* An app's boot check found updates: restart into the system settings
     * and start them with MINI_MARKET_AUTO_UPDATE. NULL: only report them */
    void (*auto_update)(void);
    /* A copy of app id built into the firmware (the system settings), or
     * NULL: it counts as installed until a newer one is in /spiffs/apps */
    const uint8_t *(*builtin)(const char *id, size_t *len);
    /* The ids of the apps the market keeps here (its folders /spiffs/apps/<id>/,
     * the root app, the built-in system apps), at most max; the count */
    int (*installed)(char (*ids)[MINI_MARKET_ID_MAX], int max);
} MiniMarketHost;

#define MINI_MARKET_TMP "/spiffs/market.tmp"

typedef enum { MINI_MARKET_IDLE, MINI_MARKET_REFRESHING, MINI_MARKET_INSTALLING, MINI_MARKET_ERROR } MiniMarketState;
/* checking: its info is being read; absent: the market has no such app (or
 * its info could not be read) */
typedef enum { MINI_MARKET_NEW, MINI_MARKET_INSTALLED, MINI_MARKET_UPDATE, MINI_MARKET_INCOMPATIBLE,
               MINI_MARKET_CHECKING, MINI_MARKET_ABSENT } MiniMarketAppStatus;
typedef enum { MINI_MARKET_AUTO_OFF, MINI_MARKET_AUTO_CHECK, MINI_MARKET_AUTO_UPDATE } MiniMarketAuto;

/* How long a boot waits for the network before the check is skipped */
#define MINI_MARKET_AUTO_WAIT_MS 30000u

void mini_market_set_host(const MiniMarketHost *host);

/** @brief The market's URL: config market_url, else <proxy>/market; "" when neither is set. */
void mini_market_url(char *out, size_t cap);

/** @brief Start checking every installed app (its <id>.info.json). false (with
 *  err) when busy, offline or no URL. */
bool mini_market_refresh(char *err, size_t cap);

/** @brief Queue the install of an app by id (one not in the list is looked up
 *  first: its info, then the image), or "updates" for every checked app with
 *  an update. Returns the number queued; 0 with err when none can be. */
int mini_market_install(const char *id, char *err, size_t cap);

/** @brief Check every installed app and install the updates. Where installs
 *  may not run (an app on the board) the host restarts into the system
 *  settings for it, which return to the app afterwards. 1 started, 2
 *  restarting, 0 with err. */
int mini_market_update_all(char *err, size_t cap);

void mini_market_cancel(void);


/** @brief Advance downloads and checks; call often from the main loop. */
void mini_market_poll(void);

/** @brief Once per boot: MINI_MARKET_AUTO_CHECK in an app (nothing unless
 *  market_auto_update = 1; app_id: the running app, "" when the market does
 *  not keep it), MINI_MARKET_AUTO_UPDATE in the system settings started by
 *  the host's auto_update. */
void mini_market_auto_start(MiniMarketAuto mode, const char *app_id);

/** @brief The automatic round; call from the main loop with the time since boot. */
void mini_market_auto(uint32_t now_ms);

/** @brief The running automatic round (MINI_MARKET_AUTO_OFF when none). */
MiniMarketAuto mini_market_auto_mode(void);

/** @brief An automatic update round is over: the settings go back to the app. */
bool mini_market_return(void);

/** @brief Release the app list (memory) when idle. */
void mini_market_release(void);

MiniMarketState mini_market_state(void);
const char *mini_market_message(void);

/** @brief Whether a system app installed from the market (file_meta: its
 *  metadata) runs instead of the copy built into the firmware: the same id,
 *  an API this firmware provides, and a version newer than the built-in
 *  one. false with why otherwise. */
bool mini_market_use_installed(const char *id, const char *file_meta, const uint8_t *builtin, size_t builtin_len,
                               char *why, size_t cap);

/** @brief Compares dotted versions ("1.10" > "1.9"); <0, 0 or >0. */
int mini_market_version_cmp(const char *a, const char *b);

/** @brief GET /api/market: {"state","message","url","current","received","total","queued",
 *  "install" (installs allowed now),"auto" ("off","check","update"),"return","apps":[...]}:
 *  apps are the installed ones of the last check (and any asked for by id) */
size_t mini_market_json(char *out, size_t cap);

/** @brief (market-refresh) (market-status) (market-apps) (market-install id) (market-cancel) */
void mini_register_market_lib(MiniVM *vm);

#endif /* MINISCHEME_MARKET_H */
