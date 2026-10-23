/**
 * @file minischeme_jsonfeed.h
 * @brief JSON feed: polls an HTTP URL in the background and keeps selected
 *        JSON objects, for Scheme apps that watch a web API.
 *
 * The document is parsed as a stream (no copy of it is held). Every JSON
 * object is a candidate: Scheme names the keys to capture and the rules that
 * select objects; selected objects' captured values are published as a
 * snapshot once the whole document has been read. What the data means is
 * entirely up to the Scheme app.
 *
 *   (json-feed-start url interval-ms keys selectors [pool-bytes [value-len [depth]]])
 *       keys:      list of key names captured per object, e.g. ("Text" "Value")
 *       selectors: list of (key-index mode string [keep-key ...]), mode
 *                  'equal or 'prefix; an object is kept when its captured key
 *                  matches, tagged with the first selector it matches; only
 *                  the keep-keys are stored for it (all keys when omitted)
 *       pool-bytes, value-len, depth: limits the app chooses (omitted or 0:
 *                  none). Nothing else is fixed: keys, selectors, rows,
 *                  values, levels and the URL are allocated as needed; on
 *                  the ESP32 the feed stops growing 16 KB above an empty heap
 *   (json-feed-stop)
 *   (json-feed-seq)          snapshot number, 0 until the first good fetch
 *   (json-feed-count)        objects kept in the snapshot
 *   (json-feed-selector i)   selector index object i matched
 *   (json-feed-field i k)    captured value of key k (an index or a name) for object i, or #f
 *   (json-feed-status)       alist: state, error, fetches, failures, bytes,
 *                            fetch-ms, age-ms, objects, truncated, memory
 *   (json-feed-url)          configured URL, or #f
 *   (json-feed-dump)         every object of the last document with any
 *                            captured key: values joined by TAB, one per line
 *
 * On the ESP32 a FreeRTOS task fetches the URL with esp_http_client; elsewhere
 * (WASM, host tests) the host pushes the document with mini_json_feed_*().
 */

#ifndef MINISCHEME_JSONFEED_H
#define MINISCHEME_JSONFEED_H

#include "minischeme.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void mini_register_json_feed_lib(MiniVM *vm);

/* Push a document in pieces (WASM proxy, tests) */
void mini_json_feed_begin(void);
void mini_json_feed_data(const char *data, size_t len);
void mini_json_feed_end(bool ok, const char *error);

/* Configured URL ("" when idle), for hosts that fetch it themselves */
const char *mini_json_feed_url(void);

/* The protocol converter for https:// feeds (config.ini [app] https_proxy,
 * else the app proxy): the board speaks plain HTTP, so json-feed-start sends
 * an https:// URL as <base>/fetch?url=<it, percent-encoded>. "" turns it off. */
void mini_json_feed_set_https_proxy(const char *base);
/* The feed's polling interval in ms (0: no feed), for hosts that fetch for it */
uint32_t mini_json_feed_interval(void);
/* Counts json-feed-start calls: a host that fetches for the feed (the
 * WebREPL) fetches again when it changes, even for the same URL */
uint32_t mini_json_feed_generation(void);

/* (fs-download url path) for hosts that fetch for it (the WebREPL): the
 * queued URL (marked as running), NULL when none; the full path the body
 * goes to and its size limit; then mini_download_done with the outcome
 * (the host writes the file itself). The ESP32 downloads in the feed's task. */
const char *mini_download_take(void);
const char *mini_download_path(void);
uint32_t mini_download_max(void);
void mini_download_done(bool ok, uint32_t bytes, const char *error);

#ifdef __cplusplus
}
#endif

#endif /* MINISCHEME_JSONFEED_H */
