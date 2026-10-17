/**
 * @file main.c
 * @brief ESP-IDF v6.1 Application Entry for MiniScheme on the ILI9488 desktop display.
 *
 * Remote execution channels (all run on the main task, the only VM owner):
 * - UART0 line protocol: "@INFO", "@EXEC <base64 msbc>", "@BEGIN <size> [path]",
 *   "@DATA <base64>", "@END [run|save]", "@EVAL <base64 source>", "@RESET",
 *   and the file manager: "@LS", "@APPS", "@READ <offset> <path>", "@RM <path>",
 *   "@MV <from> <to>". Every reply is a single "@OK {json}" or "@ERR {json}"
 *   line. Other lines form a text REPL.
 * - HTTP (port 80): GET /api/info, POST /api/exec (raw MSBC),
 *   POST /api/execute_bytecode ({"bytecode_hex": ...}), POST /api/eval,
 *   POST /api/upload?path=/spiffs/app.msbc&run=1, POST /api/reset, and the
 *   file manager: GET /api/fs/list, GET /api/fs/read?path=,
 *   POST /api/fs/delete?path=, POST /api/fs/rename?from=&to=. CORS enabled.
 *
 * Boot app, by suffix (installing one removes the other):
 * - /spiffs/app.msbc: bytecode. Built into a Code ROM image in the approm
 *   partition and run in place from Flash ([memory] app_xip = 0: load to RAM).
 * - /spiffs/app.fasl: a heap snapshot (dump-fasl), restored into the VM heap;
 *   the app restarts its C-side services in (app-resume).
 */

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <dirent.h>
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "lwip/sockets.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "lfsvfs.h"
#include "esp_partition.h"
#include "esp_attr.h"
#include <stdarg.h>
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_mac.h"
#include "esp_rom_sys.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"

#include "minischeme_api.h"
#include "minischeme_market.h"
#include "minischeme_channel.h"
#include "minischeme_jsonfeed.h"
#include "market_host.h"
#include "minischeme.h"
#include "minischeme_internal.h"
#include "minischeme_lib.h"
#include "minischeme_gui.h"
#include "minischeme_fontrom.h"
#include "minischeme_mirror.h"
#include "minischeme_fonts.h"
#include "minischeme_ili9488.h"
#include "minischeme_help.h"
#include "minischeme_device.h"
#include "minischeme_jsonfeed.h"
#include "msbc.h"
#include "app_settings_msbc.h"
#include "setup_portal.h"
#include "setup_page_html.h"

/* Set to 1 to load the Scheme compiler on the device (needs ~70KB extra heap) */
#ifndef MINISCHEME_FW_COMPILER
#define MINISCHEME_FW_COMPILER 0
#endif
/* Remote REPL transports (set to 0 to measure or drop their cost) */
#ifndef MINISCHEME_FW_HTTP
#define MINISCHEME_FW_HTTP 1
#endif
#if MINISCHEME_FW_COMPILER
#include "compiler_msbc.h"
#endif

#define REMOTE_PROTO_VERSION 1
#define APP_MSBC_PATH        "/spiffs/app.msbc"  /* bytecode: run from Flash ROM */
#define APP_FASL_PATH        "/spiffs/app.fasl"  /* heap snapshot: load to RAM */
#define APP_ROM_SIG_PATH     "/spiffs/app.romsig"
#define UPLOAD_TMP_PATH      "/spiffs/upload.tmp"
#define APPS_DIR             "/spiffs/apps"       /* installed apps and their <id>.ini */
#define USER_DIR             "/spiffs/user"       /* apps the user uploaded: /spiffs/user/<id>/ */
#define DEFAULT_DEVICE_NAME  "\xe6\xa1\x8c\xe9\x9d\xa2\xe5\xb1\x8f"   /* the web page's title until it is named */
/* The system settings from the app market: run instead of the built-in copy
 * (app_settings_msbc.h, the bootstrap) when valid and at least as new */
#define SETTINGS_APP_PATH    APPS_DIR "/settings/app.msbc"
static char s_app_dir[64] = "";             /* the running app's folder (minischeme_device.h) */
static const char *s_settings_from = "built-in";   /* which settings run: built-in or market */
/* The running app's own web page (minischeme_device.h): offset and size of
 * its gzip'd bytes in the image file, 0 when it has none */
static long s_app_web_at = 0;
static size_t s_app_web_len = 0;
#define APP_ROM_SUBTYPE      0x40                 /* "approm" partition, see partitions.csv */

static char s_app_path[64] = APP_MSBC_PATH; /* the app this boot runs ([system] boot_app) */
static char s_app_id[32] = "app";
static char s_app_name[64] = "";            /* its display name (metadata) */
static char s_rom_err[120] = "";            /* why the last image build failed */
static char s_app_ini[80] = "";
static char s_app_api_err[128] = "";         /* why the boot app cannot run on this API, "" when it can */             /* its own config: /spiffs/apps/<id>.ini */

/* A mid press on the hardware pages restarts into the system settings (app_settings_msbc.h)
 * instead of the boot app; the flag survives the restart in RTC memory */
#define SETTINGS_BOOT_MAGIC 0x5E7714C5u
static RTC_NOINIT_ATTR uint32_t s_settings_boot;
static bool s_settings_mode = false;

/* The app market's automatic update across restarts (RTC memory): UPDATE
 * makes the settings install the updates, SKIP keeps the app boot after
 * that round from checking again */
/* After a factory reset the next settings boot runs the built-in copy, never
 * one from the market (a broken one could lock the board out) */
#define BUILTIN_SETTINGS_MAGIC 0xB1E7C0DEu
static RTC_NOINIT_ATTR uint32_t s_builtin_boot;
static bool s_builtin_settings = false;          /* this boot: the built-in settings only */
static volatile int64_t s_factory_restart_at = 0;   /* a reset is done: restart into them */
#define MARKET_AUTO_UPDATE_MAGIC 0xA07D0A7Eu
#define MARKET_AUTO_SKIP_MAGIC   0xA07D5C1Fu
static RTC_NOINIT_ATTR uint32_t s_market_auto;
static volatile int s_settings_saved = 0;    /* saves from the settings page */

/* The app's memory when KEY 18 was held: the settings run instead of it, so
 * their own VM figures say nothing about the app. When the app failed
 * (phase "rom", "load" or "crash"), why: the settings show it first. */
#define LAST_APP_MAGIC 0x1A57A9A0u
typedef struct {
    uint32_t magic;
    char id[32];
    uint32_t heap, live, peak, alloc_kb, rom, uptime_s, free_heap, min_free;
    char phase[8];
    char error[120];
} LastAppStats;
static RTC_NOINIT_ATTR LastAppStats s_last_app;
static const esp_partition_t *s_rom_part;   /* the App ROM partition (defined with the ROM code) */

static bool file_has_magic(const char *path, const char *magic);
static bool root_app_shadowed(const char *path);
static bool config_save(const char *section, const char *const *keys, const char *const *values, int n);
static void configured_boot_path(char *out, size_t cap);
static char *app_json_alloc(const char *path, const char *boot, bool with_meta);
#define EXEC_MAX_BYTES       (24 * 1024)
/* JSON reply buffer: fits a full (chip-info)-style report */
#define RESULT_MAX_BYTES     3584
/* WiFi association, lwIP, SNTP and httpd allocate after the VM is created;
 * measured on hardware they need ~36KB, so never reserve less than this. */
/* Heap kept free for the system at run time (WiFi, lwIP, the HTTP server and
 * its connections, the JSON feed). Measured with the probe after WiFi joined
 * and the VM's word-only structures in IRAM (mini_word_alloc: stack, frames,
 * GC arrays, the global cache, the display list, JSON feed offsets; ~17 KB):
 * 64 KB gives a 101 KB VM heap with ~34 KB free when idle and ~19 KB at the
 * lowest under web load. 60 KB still kept 15 KB but served half the requests
 * (the HTTP server starves); without IRAM, 64 KB fell to 8 KB under load. */
#define MIN_OS_RESERVE       (64 * 1024)
#define WIFI_PROBE_WAIT_MS   4000        /* the heap probe waits this long for WiFi to join */
/* Largest block claimed at boot for the VM heap (128 KB heap + VM struct/eden) */
#define VM_HEAP_HOLD_MAX     (136 * 1024)
#define OS_INIT_RESERVE      (56 * 1024)  /* free heap WiFi + HTTP need while starting */
/* Defaults of the [display] / [memory] options in config.ini */
#define CONFIG_VERSION        3
#define DEF_BAND_LINES        13                    /* 320 px: 8.3 KB band + 16.6 KB DMA */
#define DEF_LCD_CLOCK_HZ      (10 * 1000 * 1000)    /* safe DMA clock across parallel ILI9488 panels */
#define DEF_MAX_HEAP          (128 * 1024)
#define DEF_VM_EDEN           (4 * 1024)
#define DEF_VM_STACK          1024
#define DEF_VM_FRAMES         256
#define DEF_HTTP_STACK        6144

static const char *TAG = "MiniScheme_ESP32";

static MiniVM *s_vm = NULL;
static bool s_has_compiler = false;

/* ========================================================================= */
/* Bytecode Execution Helpers                                                */
/* ========================================================================= */

static Value run_code_value(MiniVM *vm, Value code_val) {
    if (mini_get_error(vm) || !mini_is_code(code_val)) return mini_void();
    mini_push_root(vm, &code_val);
    Value clo_val = mini_make_closure(vm, code_val, mini_nil());
    mini_pop_root(vm);
    if (mini_get_error(vm)) return mini_void();
    mini_push_root(vm, &clo_val);
    Value res = mini_vm_run(vm, clo_val, 0, NULL);
    mini_pop_root(vm);
    return res;
}

static Value execute_msbc_chunk(MiniVM *vm, const uint8_t *buf, size_t len) {
    if (len >= 4 && memcmp(buf, "MFAS", 4) == 0) {
        bool ok = mini_load_fasl_mem(vm, buf, len);
        if (ok && !mini_get_error(vm)) mini_device_resume_app(vm);
        return (!ok || mini_get_error(vm)) ? mini_void() : mini_true();
    }
    if (len < 4 || memcmp(buf, "MSBC", 4) != 0) {
        vm_error(vm, "payload is not an MSBC image");
        return mini_void();
    }
    return run_code_value(vm, msbc_deserialize(vm, buf, len));
}

/* Streams MSBC from a file so large apps never need a RAM copy */
static Value execute_msbc_file(MiniVM *vm, FILE *fp) {
    uint8_t header[4];
    if (fread(header, 1, 4, fp) != 4) {
        vm_error(vm, "file too short");
        return mini_void();
    }
    fseek(fp, 0, SEEK_SET);
    if (memcmp(header, "MFAS", 4) == 0) {
        /* streamed straight into the VM heap, no RAM copy of the image */
        bool ok = mini_load_fasl_fp(vm, fp);
        if (ok && !mini_get_error(vm)) mini_device_resume_app(vm);
        return (!ok || mini_get_error(vm)) ? mini_void() : mini_true();
    }
    return run_code_value(vm, msbc_deserialize_file(vm, fp));
}

static Value execute_path(MiniVM *vm, const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        vm_error(vm, "cannot open %s", path);
        return mini_void();
    }
    Value res = execute_msbc_file(vm, fp);
    fclose(fp);
    return res;
}

static void format_scheme_val(MiniVM *vm, Value v, char *buf, size_t max_len, size_t *pos) {
    if (*pos >= max_len - 1) return;
    if (mini_is_fixnum(v)) {
        *pos += snprintf(buf + *pos, max_len - *pos, "%lld", (long long)mini_to_fixnum(v));
    } else if (mini_is_bigint(v)) {
        char *str = mini_bigint_to_str(v, 10);
        if (str) {
            *pos += snprintf(buf + *pos, max_len - *pos, "%s", str);
            free(str);
        }
    } else if (v == MINI_CONST_NIL) {
        *pos += snprintf(buf + *pos, max_len - *pos, "()");
    } else if (v == MINI_CONST_TRUE) {
        *pos += snprintf(buf + *pos, max_len - *pos, "#t");
    } else if (v == MINI_CONST_FALSE) {
        *pos += snprintf(buf + *pos, max_len - *pos, "#f");
    } else if (v == MINI_CONST_VOID) {
        *pos += snprintf(buf + *pos, max_len - *pos, "#<void>");
    } else if (mini_is_symbol(v)) {
        *pos += snprintf(buf + *pos, max_len - *pos, "%s", mini_symbol_name(v));
    } else if (mini_is_string(v)) {
        size_t lit = mini_format_string_literal(buf + *pos, max_len - *pos, v);
        *pos += lit < max_len - *pos ? lit : max_len - *pos - 1;   /* cut: stay inside buf */
    } else if (mini_is_flonum(v)) {
        *pos += snprintf(buf + *pos, max_len - *pos, "%g", mini_to_flonum(v));
    } else if (mini_is_closure(v) || mini_is_cfunc(v)) {
        *pos += snprintf(buf + *pos, max_len - *pos, "#<procedure>");
    } else if (mini_is_vector(v)) {
        *pos += snprintf(buf + *pos, max_len - *pos, "#(");
        size_t len = mini_vector_length(v);
        for (size_t i = 0; i < len && *pos < max_len - 2; i++) {
            if (i > 0) *pos += snprintf(buf + *pos, max_len - *pos, " ");
            format_scheme_val(vm, mini_vector_ref(vm, v, i), buf, max_len, pos);
        }
        *pos += snprintf(buf + *pos, max_len - *pos, ")");
    } else if (mini_is_pair(v)) {
        *pos += snprintf(buf + *pos, max_len - *pos, "(");
        Value curr = v;
        bool first = true;
        while (mini_is_pair(curr) && *pos < max_len - 2) {
            if (!first) *pos += snprintf(buf + *pos, max_len - *pos, " ");
            first = false;
            format_scheme_val(vm, mini_car(curr), buf, max_len, pos);
            curr = mini_cdr(curr);
        }
        if (!mini_is_nil(curr) && *pos < max_len - 4) {
            *pos += snprintf(buf + *pos, max_len - *pos, " . ");
            format_scheme_val(vm, curr, buf, max_len, pos);
        }
        *pos += snprintf(buf + *pos, max_len - *pos, ")");
    } else {
        *pos += snprintf(buf + *pos, max_len - *pos, "#<object>");
    }
    if (*pos >= max_len) *pos = max_len - 1;
}

/* ========================================================================= */
/* Encoding Utilities (Base64, Hex, JSON)                                    */
/* ========================================================================= */

static int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

/* Decodes base64 in place-safe fashion; returns decoded length or -1 */
static int b64_decode(const char *in, size_t in_len, uint8_t *out, size_t out_cap) {
    uint32_t acc = 0;
    int bits = 0;
    size_t n = 0;
    for (size_t i = 0; i < in_len; i++) {
        char c = in[i];
        if (c == '=' || c == '\r' || c == '\n' || c == ' ') continue;
        int v = b64_val(c);
        if (v < 0) return -1;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= out_cap) return -1;
            out[n++] = (uint8_t)(acc >> bits);
        }
    }
    return (int)n;
}

/* Encodes len bytes as base64; out needs 4 * ceil(len / 3) + 1 bytes */
static size_t b64_encode(const uint8_t *in, size_t len, char *out) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t n = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < len) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < len) v |= in[i + 2];
        out[n++] = tbl[(v >> 18) & 63];
        out[n++] = tbl[(v >> 12) & 63];
        out[n++] = i + 1 < len ? tbl[(v >> 6) & 63] : '=';
        out[n++] = i + 2 < len ? tbl[v & 63] : '=';
    }
    out[n] = '\0';
    return n;
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static size_t json_escape(const char *src, char *dst, size_t cap) {
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p && n + 7 < cap; p++) {
        if (*p == '"' || *p == '\\') { dst[n++] = '\\'; dst[n++] = (char)*p; }
        else if (*p == '\n') { dst[n++] = '\\'; dst[n++] = 'n'; }
        else if (*p == '\r') { dst[n++] = '\\'; dst[n++] = 'r'; }
        else if (*p == '\t') { dst[n++] = '\\'; dst[n++] = 't'; }
        else if (*p < 0x20) { n += (size_t)snprintf(dst + n, cap - n, "\\u%04x", *p); }
        else dst[n++] = (char)*p;
    }
    dst[n] = '\0';
    return n;
}

static const char *gui_mode_name(MiniGuiMode mode) {
    return mode == MINI_GUI_MODE_BAND ? "band" : (mode == MINI_GUI_MODE_FULL ? "full" : "direct");
}

/* ========================================================================= */
/* Remote Job Service (shared by UART frames and HTTP)                       */
/* ========================================================================= */

typedef enum {
    JOB_INFO,
    JOB_EXEC,       /* buf holds an MSBC / FASL image */
    JOB_EVAL,       /* buf holds UTF-8 source (needs on-device compiler) */
    JOB_RUN_FILE,   /* path names an MSBC file on SPIFFS */
    JOB_SCREEN,     /* changed screen rows for a mirror (binary reply) */
    JOB_STATS,      /* memory, display and load figures */
    JOB_DISPLAY,    /* color bits / scheme from the WebREPL, saved to config.ini */
    JOB_HARDWARE,   /* everything about the board for the settings page */
    JOB_MARKET,     /* the app market: path = action (status, refresh, install:<id>, cancel) */
    JOB_APP_CALL,   /* the app's web page: path = name, buf = body; (app-web name body) */
    JOB_APP_FILES,  /* the app's files changed (/api/app/file): images opened again, a repaint */
} JobKind;

typedef struct {
    JobKind kind;
    const uint8_t *buf;
    size_t len;
    char path[64];
    char *out;      /* JSON reply */
    size_t out_cap;
    size_t out_len; /* binary replies (JOB_SCREEN): bytes in out */
    bool full;      /* JOB_SCREEN: send the whole screen */
    bool since_set; /* JOB_SCREEN: the viewer's own cursor (since, from) */
    uint32_t since;
    int from;
    int bits;       /* JOB_DISPLAY: color bits (0: unchanged) */
    int scheme;     /* JOB_DISPLAY: color scheme (-1: unchanged) */
    bool failed;    /* JOB_APP_CALL: the hook failed (out is the JSON error) */
} RemoteJob;

/* WebREPL input (POST /api/input): KEY 18 and the light sensor of the page.
 * A held button is renewed by the page; when renewals stop it counts as
 * released after REMOTE_BUTTON_HOLD_US. Each press is also counted, so a tap
 * shorter than one main loop pass still reaches the app. */
#define REMOTE_BUTTON_HOLD_US 3000000
static volatile bool s_remote_btn = false;
static volatile int64_t s_remote_btn_until = 0;
static volatile uint32_t s_remote_presses = 0;
static volatile int s_light_override = -1;   /* -1: the real sensor */
static volatile bool s_light_dirty = false;

static void job_display(RemoteJob *job);

/* Load figures of the main loop, refreshed every second */
typedef struct {
    int64_t render_us;     /* last frame: render + panel */
    int64_t lcd_us;        /* last frame: panel bus */
    int render_rows;       /* last frame: rows */
    int fps_x10;           /* frames per second x 10 (apps redraw about once a second) */
    int rows_per_s;
    int busy_pct;          /* share of the second the main loop worked */
} LoopStats;

static LoopStats s_loop;
static int s_frames_acc = 0, s_rows_acc = 0;
static int64_t s_busy_acc = 0, s_stats_t0 = 0;
static void job_reply_stats(RemoteJob *job);

static SemaphoreHandle_t s_job_lock;   /* serializes HTTP clients */
static SemaphoreHandle_t s_job_done;
static RemoteJob *volatile s_pending_job = NULL;

static void job_reply_error(RemoteJob *job, const char *msg) {
    char esc[256];
    json_escape(msg, esc, sizeof(esc));
    snprintf(job->out, job->out_cap, "{\"success\":false,\"error\":\"%s\"}", esc);
}

static void job_reply_info(RemoteJob *job) {
    time_t now = time(NULL);
    char ip[32];
    snprintf(ip, sizeof(ip), "%s", mini_string_data(mini_call_named(s_vm, "wifi-ip", 0, NULL)));
    bool wifi = mini_is_true(mini_call_named(s_vm, "wifi-connected?", 0, NULL));
    mini_clear_error(s_vm);
    snprintf(job->out, job->out_cap,
             "{\"success\":true,\"proto\":%d,\"device\":\"%s\",\"compiler\":%s,"
             "\"free_heap\":%u,\"largest_block\":%u,\"heap_allocated\":%u,\"heap_capacity\":%u,"
             "\"gui_mode\":\"%s\",\"width\":%d,\"height\":%d,\"band_lines\":%d,"
             "\"wifi\":%s,\"ip\":\"%s\",\"time_synced\":%s,\"epoch\":%lld,\"uptime_ms\":%lld,"
             "\"api\":\"%s\",\"api_extensions\":[\"esp32\"]}",
             REMOTE_PROTO_VERSION, "esp32-ili9488", s_has_compiler ? "true" : "false",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned)mini_get_allocated_bytes(s_vm), (unsigned)mini_get_heap_size(s_vm),
             gui_mode_name(mini_gui_get_mode()),
             mini_gui_get_width(), mini_gui_get_height(), mini_gui_get_band_lines(),
             wifi ? "true" : "false", ip,
             hal_time_synced() ? "true" : "false",
             (long long)now, (long long)(esp_timer_get_time() / 1000), mini_api_version());
}

/* The IRAM-only heap (word access only): what holds 32-bit words but not
 * bytes. It takes the VM's word structures (mini_word_alloc) */
static void iram_heap(unsigned *total, unsigned *free_bytes) {
    *total = (unsigned)(heap_caps_get_total_size(MALLOC_CAP_32BIT) - heap_caps_get_total_size(MALLOC_CAP_8BIT));
    *free_bytes = (unsigned)(heap_caps_get_free_size(MALLOC_CAP_32BIT) - heap_caps_get_free_size(MALLOC_CAP_8BIT));
}

/* What the boot's VM heap was sized from (GET /api/hardware: vm.probe) */
static struct { uint32_t free_heap, largest, reserved; bool wifi; } s_probe;

/* Real figures for the WebREPL panel (GET /api/stats) */
static void job_reply_stats(RemoteJob *job) {
    unsigned iram_total, iram_free;
    iram_heap(&iram_total, &iram_free);
    uint8_t r = 0, g = 0, b = 0;
    mini_ws2812_get(&r, &g, &b);
    Value rssi = mini_call_named(s_vm, "wifi-rssi", 0, NULL);
    bool wifi = mini_is_true(mini_call_named(s_vm, "wifi-connected?", 0, NULL));
    char ip[32];
    snprintf(ip, sizeof(ip), "%s", mini_string_data(mini_call_named(s_vm, "wifi-ip", 0, NULL)));
    mini_clear_error(s_vm);
    snprintf(job->out, job->out_cap,
             "{\"success\":true,\"uptime_ms\":%lld,"
             "\"vm_heap_allocated\":%u,\"vm_heap_capacity\":%u,"
             "\"free_heap\":%u,\"min_free_heap\":%u,\"largest_block\":%u,\"internal_total\":%u,"
             "\"gui_mode\":\"%s\",\"color_bits\":%d,\"palette\":\"%s\",\"band_lines\":%d,"
             "\"width\":%d,\"height\":%d,\"buffer_bytes\":%u,\"dma_bytes\":%u,"
             "\"render_us\":%lld,\"lcd_us\":%lld,\"render_rows\":%d,\"fps\":%d.%d,\"rows_per_s\":%d,"
             "\"busy_pct\":%d,\"backlight\":%u,\"ws2812\":[%u,%u,%u],\"light\":%d,\"light_remote\":%s,\"button\":%s,"
             "\"wifi\":%s,\"rssi\":%d,\"ip\":\"%s\",\"fontrom_used\":%u,\"fontrom_capacity\":%u,"
             "\"vm_iram\":%u,\"iram_free\":%u,\"iram_total\":%u}",
             (long long)(esp_timer_get_time() / 1000),
             (unsigned)mini_get_allocated_bytes(s_vm), (unsigned)mini_get_heap_size(s_vm),
             (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_total_size(MALLOC_CAP_INTERNAL),
             gui_mode_name(mini_gui_get_mode()), mini_gui_get_color_bits(),
             mini_gui_scheme_name(mini_gui_get_scheme()), mini_gui_get_band_lines(),
             mini_gui_get_width(), mini_gui_get_height(),
             (unsigned)mini_gui_get_fb_ram(), (unsigned)esp32_ili9488_dma_ram_bytes(),
             (long long)s_loop.render_us, (long long)s_loop.lcd_us, s_loop.render_rows,
             s_loop.fps_x10 / 10, s_loop.fps_x10 % 10, s_loop.rows_per_s, s_loop.busy_pct,
             (unsigned)mini_backlight_get(), (unsigned)r, (unsigned)g, (unsigned)b, mini_light_read(),
             s_light_override >= 0 ? "true" : "false", mini_button_read() ? "true" : "false",
             wifi ? "true" : "false", mini_is_fixnum(rssi) ? (int)mini_to_fixnum(rssi) : 0, ip,
             (unsigned)mini_fontrom_used(), (unsigned)mini_fontrom_capacity(),
             /* the VM's word-only structures in IRAM; the IRAM-only heap */
             (unsigned)mini_word_iram_bytes(), iram_free, iram_total);
}

/* Appends to job->out; false once it is full */
static bool hw_put(RemoteJob *job, size_t *n, const char *fmt, ...) {
    if (*n >= job->out_cap) return false;
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(job->out + *n, job->out_cap - *n, fmt, ap);
    va_end(ap);
    if (k < 0 || *n + (size_t)k >= job->out_cap) {
        *n = job->out_cap;
        return false;
    }
    *n += (size_t)k;
    return true;
}

static int64_t named_int(const char *name) {
    Value v = mini_call_named(s_vm, name, 0, NULL);
    mini_clear_error(s_vm);
    return mini_is_fixnum(v) ? mini_to_fixnum(v) : 0;
}

static size_t count_files(const char *dir_path) {
    size_t n = 0;
    DIR *dir = opendir(dir_path);
    for (struct dirent *de = dir ? readdir(dir) : NULL; de; de = readdir(dir)) {
        char path[96];
        struct stat st;
        if (snprintf(path, sizeof(path), "%s/%s", dir_path, de->d_name) >= (int)sizeof(path)) continue;
        if (stat(path, &st) == 0 && !S_ISDIR(st.st_mode)) n++;
    }
    if (dir) closedir(dir);
    return n;
}

static bool sta_status(char *ssid, size_t ssid_cap, char *ip, size_t ip_cap, int *rssi);

/* GET /api/hardware: chip, firmware, memory, the running program's VM, the
 * last app (when the settings run), storage, App ROM, font ROM, display,
 * network (main task: the VM figures need it) */
static void job_reply_hardware(RemoteJob *job) {
    size_t n = 0;
    char esc[160];
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    uint32_t flash = 0;
    esp_flash_get_size(NULL, &flash);
    static const char *const reasons[] = { "unknown", "power-on", "external", "software", "panic",
                                           "int-watchdog", "task-watchdog", "watchdog", "deep-sleep",
                                           "brownout", "sdio" };
    int rr = (int)esp_reset_reason();
    const esp_app_desc_t *app = esp_app_get_description();
    hw_put(job, &n, "{\"success\":true,\"chip\":{\"model\":\"ESP32\",\"revision\":%d,\"cores\":%d,\"mhz\":%u,"
           "\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"flash\":%u,\"bt\":%s}",
           chip.revision, chip.cores, (unsigned)esp_rom_get_cpu_ticks_per_us(),
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], (unsigned)flash,
           (chip.features & CHIP_FEATURE_BT) ? "true" : "false");
    json_escape(app ? app->version : "", esc, sizeof(esc));
    hw_put(job, &n, ",\"firmware\":{\"version\":\"%s\"", esc);
    json_escape(app ? app->idf_ver : "", esc, sizeof(esc));
    hw_put(job, &n, ",\"idf\":\"%s\",\"built\":\"%s %s\",\"image\":%lld,\"partition\":%lld,"
           "\"reset\":\"%s\",\"uptime_ms\":%lld,\"time_synced\":%s}",
           esc, app ? app->date : "", app ? app->time : "", (long long)named_int("flash-app-bytes"),
           (long long)named_int("flash-app-partition-bytes"), (rr >= 0 && rr < 11) ? reasons[rr] : "unknown",
           (long long)(esp_timer_get_time() / 1000), hal_time_synced() ? "true" : "false");
    unsigned iram_total, iram_free;
    iram_heap(&iram_total, &iram_free);
    hw_put(job, &n, ",\"memory\":{\"free\":%u,\"min_free\":%u,\"largest\":%u,\"internal_total\":%u,\"dma_free\":%u,"
           "\"iram_total\":%u,\"iram_free\":%u,\"iram_vm\":%u}",
           (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_total_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA), iram_total, iram_free,
           (unsigned)mini_word_iram_bytes());
    MiniStats st;
    mini_get_stats(s_vm, &st);
    json_escape(s_settings_mode ? "settings" : s_app_id, esc, sizeof(esc));
    hw_put(job, &n, ",\"vm\":{\"program\":\"%s\"", esc);
    json_escape(s_app_path, esc, sizeof(esc));
    hw_put(job, &n, ",\"path\":\"%s\",\"settings\":%s,\"heap\":%u,\"used\":%u,\"live\":%u,\"peak\":%u,"
           "\"alloc_kb\":%u,\"minor_gcs\":%u,\"major_gcs\":%u,\"stack_peak\":%u,\"stack_slots\":%u,\"rom\":%u,"
           "\"probe\":{\"free\":%u,\"largest\":%u,\"wifi\":%s,\"reserved\":%u}}",
           esc, s_settings_mode ? "true" : "false", (unsigned)mini_get_heap_size(s_vm),
           (unsigned)mini_get_allocated_bytes(s_vm), (unsigned)st.live_bytes, (unsigned)st.live_peak,
           (unsigned)(st.alloc_bytes / 1024), (unsigned)st.minor_gcs, (unsigned)st.major_gcs,
           (unsigned)st.stack_peak, (unsigned)st.stack_slots, (unsigned)st.rom_bytes,
           (unsigned)s_probe.free_heap, (unsigned)s_probe.largest, s_probe.wifi ? "true" : "false",
           (unsigned)s_probe.reserved);
    if (s_last_app.magic == LAST_APP_MAGIC) {
        char id[33];
        snprintf(id, sizeof(id), "%.32s", s_last_app.id);
        json_escape(id, esc, sizeof(esc));
        hw_put(job, &n, ",\"last_app\":{\"id\":\"%s\",\"heap\":%u,\"live\":%u,\"peak\":%u,\"alloc_kb\":%u,"
               "\"rom\":%u,\"uptime_s\":%u,\"free\":%u,\"min_free\":%u}",
               esc, (unsigned)s_last_app.heap, (unsigned)s_last_app.live, (unsigned)s_last_app.peak,
               (unsigned)s_last_app.alloc_kb, (unsigned)s_last_app.rom, (unsigned)s_last_app.uptime_s,
               (unsigned)s_last_app.free_heap, (unsigned)s_last_app.min_free);
    }
    size_t total = 0, used = 0;
    lfsvfs_info(&total, &used);
    hw_put(job, &n, ",\"storage\":{\"total\":%u,\"used\":%u,\"files\":%u,\"apps\":%u}",
           (unsigned)total, (unsigned)used, (unsigned)(count_files("/spiffs") + count_files(APPS_DIR)),
           (unsigned)count_files(APPS_DIR));
    /* the settings run from RAM and never map the App ROM: look the partition up */
    const esp_partition_t *rom = s_rom_part ? s_rom_part
        : esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)APP_ROM_SUBTYPE, NULL);
    hw_put(job, &n, ",\"app_rom\":{\"partition\":%u,\"image\":%u}",
           (unsigned)(rom ? rom->size : 0), (unsigned)st.rom_bytes);
    hw_put(job, &n, ",\"font_rom\":{\"capacity\":%u,\"used\":%u,\"fonts\":[",
           (unsigned)mini_fontrom_capacity(), (unsigned)mini_fontrom_used());
    for (int i = 0; i < mini_fontrom_count(); i++) {
        const uint8_t *data;
        size_t len;
        const char *name = mini_fontrom_entry(i, &data, &len);
        json_escape(name ? name : "", esc, sizeof(esc));
        hw_put(job, &n, "%s{\"name\":\"%s\",\"bytes\":%u}", i ? "," : "", esc, (unsigned)len);
    }
    hw_put(job, &n, "],\"slots\":[");
    for (int id = 0; id < MINI_FONT_SLOTS; id++) {
        const char *src = mini_font_source(id);
        const MiniFont *f = mini_font_get(id);
        json_escape(f && f->name ? f->name : "", esc, sizeof(esc));
        hw_put(job, &n, "%s{\"slot\":%d,\"name\":\"%s\",\"source\":\"%s\",\"height\":%d}", id ? "," : "", id, esc,
               src ? src : "built-in", f ? f->height : 0);
    }
    Value model = mini_call_named(s_vm, "display-model", 0, NULL);
    mini_clear_error(s_vm);
    json_escape(mini_is_string(model) ? mini_string_data(model) : "", esc, sizeof(esc));
    hw_put(job, &n, "]},\"display\":{\"model\":\"%s\",\"width\":%d,\"height\":%d,\"mode\":\"%s\",\"band_lines\":%d,"
           "\"color_bits\":%d,\"scheme\":\"%s\",\"buffer\":%u,\"dma\":%u,\"fps\":%d.%d,\"busy_pct\":%d,\"backlight\":%u,"
           "\"dl_cmds\":%d,\"dl_cap\":%d,\"dl_heap\":%u}",
           esc, mini_gui_get_width(), mini_gui_get_height(), gui_mode_name(mini_gui_get_mode()),
           mini_gui_get_band_lines(), mini_gui_get_color_bits(), mini_gui_scheme_name(mini_gui_get_scheme()),
           (unsigned)mini_gui_get_fb_ram(), (unsigned)esp32_ili9488_dma_ram_bytes(),
           s_loop.fps_x10 / 10, s_loop.fps_x10 % 10, s_loop.busy_pct, (unsigned)mini_backlight_get(),
           mini_gui_display_list_size(), mini_gui_display_list_capacity(),
           (unsigned)mini_gui_display_list_heap_bytes());
    char ssid[33], ip[16];
    int rssi;
    bool sta = sta_status(ssid, sizeof(ssid), ip, sizeof(ip), &rssi);
    json_escape(ssid, esc, sizeof(esc));
    hw_put(job, &n, ",\"network\":{\"connected\":%s,\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d", sta ? "true" : "false",
           esc, ip, rssi);
    json_escape(setup_portal_ssid(), esc, sizeof(esc));
    if (!hw_put(job, &n, ",\"ap\":%s,\"ap_ssid\":\"%s\",\"ap_ip\":\"%s\",\"clients\":%d}}",
                setup_portal_active() ? "true" : "false", esc, setup_portal_ip(), setup_portal_clients())) {
        job_reply_error(job, "hardware report too long");
    }
}

/* Runs on the main task. Fills job->out with a JSON object. */
static void run_job(RemoteJob *job) {
    MiniVM *vm = s_vm;
    mini_clear_error(vm);
    if (job->kind == JOB_INFO) {
        job_reply_info(job);
        return;
    }
    /* screen and stats requests leave the app alone (no repaint below) */
    if (job->kind == JOB_SCREEN) {
        if (job->since_set) {
            job->out_len = mini_mirror_fill_since(vm, (uint8_t *)job->out, job->out_cap, job->since, job->from);
        } else {
            if (job->full) mini_mirror_restart();
            job->out_len = mini_mirror_fill(vm, (uint8_t *)job->out, job->out_cap);
        }
        return;
    }
    if (job->kind == JOB_STATS) {
        job_reply_stats(job);
        return;
    }
    if (job->kind == JOB_DISPLAY) {
        job_display(job);
        return;
    }
    if (job->kind == JOB_HARDWARE) {
        job_reply_hardware(job);
        return;
    }
    if (job->kind == JOB_APP_CALL) {   /* (app-web name body): the app's VM belongs to this loop */
        if (s_settings_mode) {
            job->failed = true;
            job_reply_error(job, "no app runs (system settings)");
            return;
        }
        Value args[2] = { mini_void(), mini_void() };
        mini_push_root(vm, &args[0]);
        mini_push_root(vm, &args[1]);
        args[0] = mini_make_string(vm, job->path);
        args[1] = mini_make_string_len(vm, (const char *)(job->buf ? job->buf : (const uint8_t *)""), job->len);
        Value fn = mini_get_global(vm, "app-web");
        Value res = (mini_is_closure(fn) || mini_is_cfunc(fn)) ? mini_call(vm, fn, 2, args) : mini_void();
        mini_pop_root(vm);
        mini_pop_root(vm);
        if (mini_get_error(vm)) {
            job->failed = true;
            job_reply_error(job, mini_get_error(vm));
            mini_clear_error(vm);
        } else if (!(mini_is_closure(fn) || mini_is_cfunc(fn))) {
            job->failed = true;
            job_reply_error(job, "this app has no (app-web name body)");
        } else if (mini_is_string(res)) {
            size_t n = mini_string_len(res);
            if (n >= job->out_cap) n = job->out_cap - 1;
            memcpy(job->out, mini_string_data(res), n);
            job->out[n] = '\0';
            job->out_len = n;
        } else {
            size_t pos = 0;
            format_scheme_val(vm, res, job->out, job->out_cap, &pos);
            job->out_len = pos;
        }
        mini_gui_invalidate(0, mini_gui_get_height());   /* the call may have changed what the app shows */
        return;
    }
    if (job->kind == JOB_APP_FILES) {
        mini_gui_image_forget();
        mini_gui_invalidate(0, mini_gui_get_height());
        return;
    }
    if (job->kind == JOB_MARKET) {   /* the engine's state belongs to this loop */
        char err[112] = "";
        bool ok = true;
        if (strcmp(job->path, "refresh") == 0) ok = mini_market_refresh(err, sizeof(err));
        else if (strncmp(job->path, "install:", 8) == 0) ok = mini_market_install(job->path + 8, err, sizeof(err)) > 0;
        else if (strcmp(job->path, "cancel") == 0) mini_market_cancel();
        size_t n = mini_market_json(job->out, job->out_cap);
        if (!ok && n > 1 && n + strlen(err) + 32 < job->out_cap) {   /* "...]}" -> "...],"error":".."}" */
            snprintf(job->out + n - 1, job->out_cap - n + 1, ",\"error\":\"%s\"}", err);
        }
        return;
    }

    int64_t t0 = esp_timer_get_time();
    Value res = mini_void();
    mini_app_privileged_begin();   /* the REPL is the owner, not the app's sandbox */
    if (job->kind == JOB_EXEC) {
        res = execute_msbc_chunk(vm, job->buf, job->len);
    } else if (job->kind == JOB_RUN_FILE) {
        res = execute_path(vm, job->path);
    } else if (job->kind == JOB_EVAL) {
        if (!s_has_compiler) {
            mini_app_privileged_end();
            job_reply_error(job, "compiler not available on device; send MSBC bytecode compiled by the WebREPL");
            return;
        }
        char *src = (char *)malloc(job->len + 1);
        if (!src) {
            mini_app_privileged_end();
            job_reply_error(job, "out of memory");
            return;
        }
        memcpy(src, job->buf, job->len);
        src[job->len] = '\0';
        res = mini_eval_string(vm, src);
        free(src);
    }
    mini_app_privileged_end();
    int64_t elapsed_us = esp_timer_get_time() - t0;

    if (mini_get_error(vm)) {
        job_reply_error(job, mini_get_error(vm));
        mini_clear_error(vm);
    } else {
        /* Transient heap buffers keep the VM's main task stack free */
        const size_t val_cap = 2048, esc_cap = RESULT_MAX_BYTES - 256;
        char *val = (char *)malloc(val_cap);
        char *esc = (char *)malloc(esc_cap);
        if (!val || !esc) {
            free(val);
            free(esc);
            job_reply_error(job, "out of memory formatting result");
        } else {
            size_t pos = 0;
            val[0] = '\0';
            format_scheme_val(vm, res, val, val_cap, &pos);
            json_escape(val, esc, esc_cap);
            snprintf(job->out, job->out_cap,
                     "{\"success\":true,\"result\":\"%s\",\"exec_us\":%lld,"
                     "\"heap_allocated\":%u,\"heap_capacity\":%u}",
                     esc, (long long)elapsed_us,
                     (unsigned)mini_get_allocated_bytes(vm), (unsigned)mini_get_heap_size(vm));
            free(val);
            free(esc);
        }
    }
    /* Remote code may have changed app state: repaint the whole screen */
    mini_gui_invalidate(0, mini_gui_get_height());
}

/* Called by HTTP handlers (other task): hand the job to the main loop */
static void submit_job_and_wait(RemoteJob *job) {
    xSemaphoreTake(s_job_lock, portMAX_DELAY);
    s_pending_job = job;
    xSemaphoreTake(s_job_done, portMAX_DELAY);
    xSemaphoreGive(s_job_lock);
}

static void service_pending_job(void) {
    RemoteJob *job = s_pending_job;
    if (!job) return;
    run_job(job);
    s_pending_job = NULL;
    xSemaphoreGive(s_job_done);
}

static bool safe_spiffs_path(const char *path) {
    return strncmp(path, "/spiffs/", 8) == 0 && strstr(path, "..") == NULL && strlen(path) < 64;
}

static bool is_app_path(const char *path) {
    return strcmp(path, APP_MSBC_PATH) == 0 || strcmp(path, APP_FASL_PATH) == 0;
}

/* The newest install wins: an app.msbc and an app.fasl never coexist */
static void app_installed(const char *path) {
    remove(strcmp(path, APP_MSBC_PATH) == 0 ? APP_FASL_PATH : APP_MSBC_PATH);
    ESP_LOGI(TAG, "Boot app is now %s", path);
}

/* A new boot app starts on a clean reboot: an .msbc app is built into the
 * Flash ROM image on the way up, an .fasl snapshot replaces the whole heap */
static void restart_into_app(void) {
    vTaskDelay(pdMS_TO_TICKS(200)); /* let the reply reach the client */
    esp_restart();
}

/* ========================================================================= */
/* SPIFFS File Manager (HTTP /api/fs routes, UART @LS / @READ / @RM / @MV)    */
/* ========================================================================= */

#define FS_READ_CHUNK 768 /* raw bytes per @READ reply (1024 base64 chars) */

typedef void (*TextSink)(void *ctx, const char *text);

/* {"success":true,"boot":path,"apps":[mini_app_json ...]}: GET /api/apps and the serial "@APPS" */
static void apps_json_stream(TextSink out, void *ctx, bool with_meta);

/* Lists dir (a path under /spiffs, depth levels of subdirectories) */
static void fs_list_dir(TextSink out, void *ctx, const char *dir_path, int depth, const char **sep) {
    DIR *dir = opendir(dir_path);
    if (!dir) return;
    for (struct dirent *de = readdir(dir); de; de = readdir(dir)) {
        char path[96], esc[120], item[160];
        if (snprintf(path, sizeof(path), "%s/%s", dir_path, de->d_name) >= (int)sizeof(path)) continue;
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth > 0) fs_list_dir(out, ctx, path, depth - 1, sep);
            continue;
        }
        json_escape(path, esc, sizeof(esc));
        snprintf(item, sizeof(item), "%s{\"path\":\"%s\",\"size\":%ld}", *sep, esc, (long)st.st_size);
        out(ctx, item);
        *sep = ",";
    }
    closedir(dir);
}

/* {"success":true,"total":N,"used":N,"files":[{"path":"/spiffs/a","size":N},...]}
 * Files in subdirectories (/spiffs/fonts/x.mfnt, ...) are listed
 * by their full path. */
static void fs_list_json(TextSink out, void *ctx) {
    size_t total = 0, used = 0;
    lfsvfs_info(&total, &used);
    char item[160];
    snprintf(item, sizeof(item), "{\"success\":true,\"total\":%u,\"used\":%u,\"files\":[",
             (unsigned)total, (unsigned)used);
    out(ctx, item);
    const char *sep = "";
    fs_list_dir(out, ctx, "/spiffs", 3, &sep);
    out(ctx, "]}");
}

/* NULL on success, else an error message */
static const char *fs_remove(const char *path) {
    struct stat st;
    if (!safe_spiffs_path(path)) return "path must be under /spiffs/";
    if (stat(path, &st) != 0) return "no such file";
    return remove(path) == 0 ? NULL : "remove failed";
}

/* Renaming onto app.msbc / app.fasl makes that file the boot app */
static const char *fs_rename(const char *from, const char *to) {
    struct stat st;
    if (!safe_spiffs_path(from) || !safe_spiffs_path(to)) return "path must be under /spiffs/";
    if (stat(from, &st) != 0) return "no such file";
    if (stat(to, &st) == 0) return "target exists";
    if (rename(from, to) != 0) return "rename failed";
    if (is_app_path(to)) app_installed(to);
    return NULL;
}

/* ========================================================================= */
/* UART0 Line Protocol & Text REPL                                           */
/* ========================================================================= */

#define SERIAL_LINE_MAX 1600

static char *s_line = NULL;   /* allocated on the first byte (a serial console is rare) */
static size_t s_line_len = 0;
static bool s_line_is_frame = false;
static bool s_line_overflow = false; /* reject, never run, a truncated line */
static FILE *s_upload_fp = NULL;
static size_t s_upload_expected = 0;
static size_t s_upload_received = 0;
static char s_upload_path[64];

static void serial_reply(const char *json) {
    bool ok = strncmp(json, "{\"success\":true", 15) == 0;
    printf("%s %s\r\n", ok ? "@OK" : "@ERR", json);
    fflush(stdout);
}

static void serial_reply_error(const char *msg) {
    char buf[320];
    RemoteJob job = { .out = buf, .out_cap = sizeof(buf) };
    job_reply_error(&job, msg);
    serial_reply(buf);
}

static void serial_run(JobKind kind, const uint8_t *buf, size_t len, const char *path) {
    char *out = (char *)malloc(RESULT_MAX_BYTES);
    if (!out) {
        serial_reply_error("out of memory");
        return;
    }
    out[0] = '\0';
    RemoteJob job = { .kind = kind, .buf = buf, .len = len, .out = out, .out_cap = RESULT_MAX_BYTES };
    if (path) snprintf(job.path, sizeof(job.path), "%s", path);
    run_job(&job);
    serial_reply(out);
    free(out);
}

static void serial_sink(void *ctx, const char *text) {
    (void)ctx;
    fputs(text, stdout);
}

/* @READ <offset> <path>: up to FS_READ_CHUNK bytes from offset, base64 */
static void serial_fs_read(const char *path, long offset) {
    if (!safe_spiffs_path(path)) {
        serial_reply_error("path must be under /spiffs/");
        return;
    }
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        serial_reply_error("no such file");
        return;
    }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    if (offset < 0 || offset > size) offset = size;
    fseek(fp, offset, SEEK_SET);
    uint8_t raw[FS_READ_CHUNK];
    size_t n = fread(raw, 1, sizeof(raw), fp);
    fclose(fp);
    char b64[FS_READ_CHUNK / 3 * 4 + 4];
    b64_encode(raw, n, b64);
    printf("@OK {\"success\":true,\"size\":%ld,\"offset\":%ld,\"data\":\"%s\"}\r\n", size, offset, b64);
}

static void handle_frame(char *line) {
    char *cmd = line + 1;
    char *arg = strchr(cmd, ' ');
    if (arg) *arg++ = '\0'; else arg = cmd + strlen(cmd);

    if (strcmp(cmd, "INFO") == 0) {
        serial_run(JOB_INFO, NULL, 0, NULL);
    } else if (strcmp(cmd, "EXEC") == 0 || strcmp(cmd, "EVAL") == 0) {
        size_t cap = strlen(arg) * 3 / 4 + 4;
        uint8_t *buf = (uint8_t *)malloc(cap);
        if (!buf) { serial_reply_error("out of memory"); return; }
        int n = b64_decode(arg, strlen(arg), buf, cap);
        if (n <= 0) serial_reply_error("invalid base64 payload");
        else serial_run(cmd[1] == 'X' ? JOB_EXEC : JOB_EVAL, buf, (size_t)n, NULL);
        free(buf);
    } else if (strcmp(cmd, "BEGIN") == 0) {
        char *path = strchr(arg, ' ');
        if (path) *path++ = '\0';
        s_upload_expected = (size_t)strtoul(arg, NULL, 10);
        snprintf(s_upload_path, sizeof(s_upload_path), "%s", (path && *path) ? path : UPLOAD_TMP_PATH);
        if (!safe_spiffs_path(s_upload_path)) { serial_reply_error("path must be under /spiffs/"); return; }
        if (s_upload_fp) fclose(s_upload_fp);
        /* Stage into a temp file so a broken transfer never clobbers the app */
        s_upload_fp = fopen(UPLOAD_TMP_PATH, "wb");
        s_upload_received = 0;
        if (!s_upload_fp) { serial_reply_error("cannot open upload file"); return; }
        printf("@OK {\"success\":true,\"size\":%u}\r\n", (unsigned)s_upload_expected);
    } else if (strcmp(cmd, "DATA") == 0) {
        if (!s_upload_fp) { serial_reply_error("no upload in progress"); return; }
        uint8_t chunk[SERIAL_LINE_MAX * 3 / 4];
        int n = b64_decode(arg, strlen(arg), chunk, sizeof(chunk));
        if (n < 0 || fwrite(chunk, 1, (size_t)n, s_upload_fp) != (size_t)n) {
            fclose(s_upload_fp);
            s_upload_fp = NULL;
            serial_reply_error("upload write failed");
            return;
        }
        s_upload_received += (size_t)n;
        printf("@OK {\"success\":true,\"received\":%u}\r\n", (unsigned)s_upload_received);
    } else if (strcmp(cmd, "END") == 0) {
        if (!s_upload_fp) { serial_reply_error("no upload in progress"); return; }
        fclose(s_upload_fp);
        s_upload_fp = NULL;
        if (s_upload_expected && s_upload_received != s_upload_expected) {
            serial_reply_error("upload size mismatch");
            return;
        }
        if (strcmp(s_upload_path, UPLOAD_TMP_PATH) != 0) {
            remove(s_upload_path);
            if (rename(UPLOAD_TMP_PATH, s_upload_path) != 0) { serial_reply_error("rename failed"); return; }
        }
        bool app = is_app_path(s_upload_path);
        if (app) app_installed(s_upload_path);
        if (strcmp(arg, "save") == 0 || app) {
            bool restart = app && strcmp(arg, "save") != 0;
            printf("@OK {\"success\":true,\"saved\":\"%s\",\"size\":%u,\"restart\":%s}\r\n",
                   s_upload_path, (unsigned)s_upload_received, restart ? "true" : "false");
            fflush(stdout);
            if (restart) restart_into_app();
        } else {
            serial_run(JOB_RUN_FILE, NULL, 0, s_upload_path);
        }
    } else if (strcmp(cmd, "LS") == 0) {
        fputs("@OK ", stdout);
        fs_list_json(serial_sink, NULL);
        fputs("\r\n", stdout);
    } else if (strcmp(cmd, "APPS") == 0) {
        fputs("@OK ", stdout);
        apps_json_stream(serial_sink, NULL, true);
        fputs("\r\n", stdout);
    } else if (strcmp(cmd, "READ") == 0) {
        char *path = strchr(arg, ' ');
        if (path) *path++ = '\0';
        serial_fs_read(path ? path : "", strtol(arg, NULL, 10));
    } else if (strcmp(cmd, "RM") == 0 || strcmp(cmd, "MV") == 0) {
        char *to = strchr(arg, ' ');
        if (to) *to++ = '\0';
        const char *err = cmd[0] == 'R' ? fs_remove(arg) : fs_rename(arg, to ? to : "");
        if (err) serial_reply_error(err);
        else serial_reply("{\"success\":true}");
    } else if (strcmp(cmd, "RESET") == 0) {
        printf("@OK {\"success\":true}\r\n");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(50));
        esp_restart();
    } else {
        serial_reply_error("unknown command");
    }
    fflush(stdout);
}

static void handle_text_line(char *line) {
    if (!s_has_compiler) {
        printf("Error: no compiler on this firmware. Use the WebREPL serial mode, "
               "which compiles in the browser and sends bytecode.\r\n");
        return;
    }
    Value res = mini_eval_string(s_vm, line);
    if (mini_get_error(s_vm)) {
        printf("Error: %s\r\n", mini_get_error(s_vm));
        mini_clear_error(s_vm);
    } else if (!mini_is_void(res)) {
        char out_buf[512];
        size_t pos = 0;
        format_scheme_val(s_vm, res, out_buf, sizeof(out_buf), &pos);
        printf("=> %s\r\n", out_buf);
    }
    mini_gui_invalidate(0, mini_gui_get_height());
}

static void serial_poll(void) {
    uint8_t rx_buf[128];
    int rx_len = uart_read_bytes(UART_NUM_0, rx_buf, sizeof(rx_buf), 0);
    if (rx_len > 0 && !s_line) {
        s_line = (char *)malloc(SERIAL_LINE_MAX);
        if (!s_line) return;   /* no memory now: the bytes are dropped */
    }
    for (int b = 0; b < rx_len; b++) {
        uint8_t c = rx_buf[b];
        if (c == '\r' || c == '\n') {
            if (s_line_len == 0) continue;
            s_line[s_line_len] = '\0';
            bool frame = s_line_is_frame;
            bool overflow = s_line_overflow;
            s_line_len = 0;
            s_line_is_frame = false;
            s_line_overflow = false;
            if (overflow) {
                if (frame) serial_reply_error("line too long: use @BEGIN/@DATA/@END for large payloads");
                else printf("\r\nError: line too long\r\nminischeme> ");
            } else if (frame) {
                handle_frame(s_line);
            } else {
                printf("\r\n");
                handle_text_line(s_line);
                printf("minischeme> ");
            }
            fflush(stdout);
        } else if (s_line_len == 0 && c == '@') {
            s_line_is_frame = true; /* frames are never echoed */
            s_line[s_line_len++] = (char)c;
        } else if (!s_line_is_frame && (c == 0x08 || c == 0x7F)) {
            if (s_line_len > 0) {
                s_line_len--;
                printf("\b \b");
                fflush(stdout);
            }
        } else if (c == 0x03) {
            s_line_len = 0;
            s_line_is_frame = false;
            s_line_overflow = false;
            printf("^C\r\nminischeme> ");
            fflush(stdout);
        } else if (c >= 32 || c == '\t' || c >= 0x80) {
            if (s_line_len < SERIAL_LINE_MAX - 1) {
                s_line[s_line_len++] = (char)c;
                if (!s_line_is_frame) {
                    putchar((int)c);
                    fflush(stdout);
                }
            } else {
                s_line_overflow = true;
            }
        }
    }
}

#if MINISCHEME_FW_HTTP
/* ========================================================================= */
/* HTTP Server                                                               */
/* ========================================================================= */

static void http_cors(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    /* Chrome Private Network Access preflight */
    httpd_resp_set_hdr(req, "Access-Control-Allow-Private-Network", "true");
}

static esp_err_t http_send_json(httpd_req_t *req, const char *json) {
    http_cors(req);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t http_options(httpd_req_t *req) {
    http_cors(req);
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

/* Reads the whole body into a heap buffer (caller frees) */
static uint8_t *http_read_body(httpd_req_t *req, size_t *out_len, const char **err) {
    size_t total = req->content_len;
    if (total == 0 || total > EXEC_MAX_BYTES * 2 + 64) {
        *err = total == 0 ? "empty body" : "body too large";
        return NULL;
    }
    uint8_t *buf = (uint8_t *)malloc(total + 1);
    if (!buf) { *err = "out of memory"; return NULL; }
    size_t got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, (char *)buf + got, total - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) { free(buf); *err = "receive failed"; return NULL; }
        got += (size_t)r;
    }
    buf[got] = '\0';
    *out_len = got;
    return buf;
}

static esp_err_t http_run(httpd_req_t *req, JobKind kind, const uint8_t *buf, size_t len, const char *path) {
    /* Heap buffer: the httpd task stack is only 6KB */
    char *out = (char *)malloc(RESULT_MAX_BYTES);
    if (!out) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    out[0] = '\0';
    RemoteJob job = { .kind = kind, .buf = buf, .len = len, .out = out, .out_cap = RESULT_MAX_BYTES };
    if (path) snprintf(job.path, sizeof(job.path), "%s", path);
    submit_job_and_wait(&job);
    esp_err_t r = http_send_json(req, out);
    free(out);
    return r;
}

static esp_err_t http_error(httpd_req_t *req, const char *msg) {
    char out[320];
    RemoteJob job = { .out = out, .out_cap = sizeof(out) };
    job_reply_error(&job, msg);
    return http_send_json(req, out);
}

static esp_err_t http_info(httpd_req_t *req) {
    return http_run(req, JOB_INFO, NULL, 0, NULL);
}

static esp_err_t http_stats(httpd_req_t *req) {
    return http_run(req, JOB_STATS, NULL, 0, NULL);
}

/* GET /api/heapmap: the heap's blocks, for finding what holds RAM. Every
 * used block of at least HEAPMAP_MIN bytes (address and size, largest
 * first), then totals of the smaller ones and of the free space, for the
 * byte-addressable heap ("dram") and the IRAM-only one ("iram"). */
#define HEAPMAP_MIN  192
#define HEAPMAP_MAX  128
typedef struct {
    int n;
    uintptr_t addr[HEAPMAP_MAX];
    uint32_t size[HEAPMAP_MAX];
    uint32_t small_n, small_bytes, used_bytes, free_bytes, free_blocks, dropped;
} HeapMap;

/* Who holds a block: what the modules report, and the tasks' stacks and TCBs */
#define HEAPMAP_OWNERS 96
typedef struct {
    int n;
    const void *ptr[HEAPMAP_OWNERS];
    const char *what[HEAPMAP_OWNERS];
} HeapOwners;

static void heapmap_owner(const void *block, const char *what, void *user) {
    HeapOwners *o = (HeapOwners *)user;
    if (block && o->n < HEAPMAP_OWNERS) {
        o->ptr[o->n] = block;
        o->what[o->n++] = what;
    }
}

static const char *const HEAPMAP_TASKS[] = {
    "main", "IDLE0", "IDLE1", "ipc0", "ipc1", "esp_timer", "sys_evt", "tiT", "wifi", "httpd",
    "jsonfeed", "market", "setup_dns", "ip_geo_task", "Tmr Svc",
};
static char s_heapmap_labels[2 * sizeof(HEAPMAP_TASKS) / sizeof(HEAPMAP_TASKS[0])][40];

static void heapmap_owners(HeapOwners *o) {
    o->n = 0;
    mini_vm_mem_owners(s_vm, heapmap_owner, o);
    mini_gui_mem_owners(heapmap_owner, o);
    mini_json_feed_mem_owners(heapmap_owner, o);
    mini_font_mem_owners(heapmap_owner, o);
    mini_mirror_mem_owners(heapmap_owner, o);
    heapmap_owner(s_line, "serial line", o);
    for (size_t i = 0; i < sizeof(HEAPMAP_TASKS) / sizeof(HEAPMAP_TASKS[0]); i++) {
        TaskHandle_t t = xTaskGetHandle(HEAPMAP_TASKS[i]);
        if (!t) continue;
        /* the least stack this task ever had left */
        snprintf(s_heapmap_labels[2 * i], sizeof(s_heapmap_labels[0]), "stack %s (%u left)", HEAPMAP_TASKS[i],
                 (unsigned)uxTaskGetStackHighWaterMark(t));
        snprintf(s_heapmap_labels[2 * i + 1], sizeof(s_heapmap_labels[0]), "tcb %s", HEAPMAP_TASKS[i]);
#if (defined(INCLUDE_pxTaskGetStackStart) && INCLUDE_pxTaskGetStackStart == 1) || (defined(configUSE_TRACE_FACILITY) && configUSE_TRACE_FACILITY == 1)
        heapmap_owner(pxTaskGetStackStart(t), s_heapmap_labels[2 * i], o);
#endif
        heapmap_owner(t, s_heapmap_labels[2 * i + 1], o);
    }
}

static const char *heapmap_who(const HeapOwners *o, uintptr_t addr) {
    for (int i = 0; i < o->n; i++) if ((uintptr_t)o->ptr[i] == addr) return o->what[i];
    return "";
}

/* The IRAM-only heap: word access only (mini_word_alloc) */
static bool heapmap_is_iram(uintptr_t a) { return a >= 0x40080000u && a < 0x400A0000u; }

static void heapmap_add(HeapMap *m, walker_block_info_t block) {
    if (!block.used) {
        m->free_bytes += (uint32_t)block.size;
        m->free_blocks++;
        return;
    }
    m->used_bytes += (uint32_t)block.size;
    if (block.size < HEAPMAP_MIN) {
        m->small_n++;
        m->small_bytes += (uint32_t)block.size;
    } else if (m->n < HEAPMAP_MAX) {
        m->addr[m->n] = (uintptr_t)block.ptr;
        m->size[m->n++] = (uint32_t)block.size;
    } else {
        m->dropped++;
    }
}

static bool heapmap_walk(walker_heap_into_t heap, walker_block_info_t block, void *user) {
    HeapMap *m = (HeapMap *)user;
    heapmap_add(heapmap_is_iram((uintptr_t)heap.start) ? &m[1] : &m[0], block);
    return true;
}

static size_t heapmap_json(HeapMap *m, const HeapOwners *o, char *out, size_t cap) {
    for (int i = 1; i < m->n; i++) {   /* largest first */
        uint32_t s = m->size[i];
        uintptr_t a = m->addr[i];
        int j = i - 1;
        while (j >= 0 && m->size[j] < s) { m->size[j + 1] = m->size[j]; m->addr[j + 1] = m->addr[j]; j--; }
        m->size[j + 1] = s;
        m->addr[j + 1] = a;
    }
    size_t n = (size_t)snprintf(out, cap, "{\"used\":%u,\"free\":%u,\"free_blocks\":%u,\"small\":[%u,%u],"
                                "\"dropped\":%u,\"blocks\":[", (unsigned)m->used_bytes, (unsigned)m->free_bytes,
                                (unsigned)m->free_blocks, (unsigned)m->small_n, (unsigned)m->small_bytes,
                                (unsigned)m->dropped);
    for (int i = 0; i < m->n && n + 32 < cap; i++) {
        n += (size_t)snprintf(out + n, cap - n, "%s[\"%08x\",%u,\"%s\"]", i ? "," : "", (unsigned)m->addr[i],
                              (unsigned)m->size[i], heapmap_who(o, m->addr[i]));
    }
    n += (size_t)snprintf(out + n, cap - n, "]}");
    return n;
}

static esp_err_t http_heapmap(httpd_req_t *req) {
    HeapMap *m = (HeapMap *)calloc(2, sizeof(HeapMap));   /* [0] DRAM, [1] IRAM-only */
    HeapOwners *o = (HeapOwners *)malloc(sizeof(HeapOwners));
    char *out = (char *)malloc(10240);
    if (!m || !o || !out) {
        free(m);
        free(o);
        free(out);
        return http_error(req, "out of memory");
    }
    heapmap_owners(o);
    heap_caps_walk_all(heapmap_walk, m);
    size_t n = (size_t)snprintf(out, 10240, "{\"success\":true,\"dram\":");
    n += heapmap_json(&m[0], o, out + n, 10240 - n - 1400);
    n += (size_t)snprintf(out + n, 10240 - n, ",\"iram\":");
    n += heapmap_json(&m[1], o, out + n, 10240 - n - 4);
    snprintf(out + n, 10240 - n, "}");
    esp_err_t r = http_send_json(req, out);
    free(m);
    free(o);
    free(out);
    return r;
}

/* GET /api/screen?since=V&from=Y: the rows changed after version V, from
 * row Y down, packed (minischeme_mirror.h); each viewer keeps its own V, so
 * several viewers all stay complete. Without since (an old WebREPL; full=1
 * starts over): one cursor shared by such viewers. The reply holds what fits
 * an 8 KB buffer (kept small: WiFi needs the heap) and flags whether more
 * rows wait. */
static esp_err_t http_screen(httpd_req_t *req) {
    char query[64] = {0}, full[4] = "0", since[12] = "", from[8] = "";
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "full", full, sizeof(full));
        httpd_query_key_value(query, "since", since, sizeof(since));
        httpd_query_key_value(query, "from", from, sizeof(from));
    }
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    size_t cap = largest > 16 * 1024 ? 8 * 1024 : (largest > 8 * 1024 ? largest - 6 * 1024 : 0);
    size_t need = MINI_MIRROR_HEADER + mini_mirror_row_max(mini_gui_get_width());
    char *out = cap >= need ? (char *)malloc(cap) : NULL;
    if (!out) return http_error(req, "out of memory for the screen buffer");
    RemoteJob job = { .kind = JOB_SCREEN, .out = out, .out_cap = cap, .full = full[0] == '1',
                      .since_set = since[0] != '\0', .since = (uint32_t)strtoul(since, NULL, 10),
                      .from = (int)strtol(from, NULL, 10) };
    submit_job_and_wait(&job);
    http_cors(req);
    httpd_resp_set_type(req, "application/octet-stream");
    esp_err_t r = httpd_resp_send(req, out, (ssize_t)job.out_len);
    free(out);
    return r;
}

static esp_err_t http_exec_raw(httpd_req_t *req) {
    const char *err = NULL;
    size_t len = 0;
    uint8_t *body = http_read_body(req, &len, &err);
    if (!body) return http_error(req, err);
    esp_err_t r = http_run(req, JOB_EXEC, body, len, NULL);
    free(body);
    return r;
}

static esp_err_t http_eval(httpd_req_t *req) {
    const char *err = NULL;
    size_t len = 0;
    uint8_t *body = http_read_body(req, &len, &err);
    if (!body) return http_error(req, err);
    esp_err_t r = http_run(req, JOB_EVAL, body, len, NULL);
    free(body);
    return r;
}

/* Compatible with web_repl/server.py: {"bytecode_hex": "4d534243..."} */
static esp_err_t http_exec_hex(httpd_req_t *req) {
    const char *err = NULL;
    size_t len = 0;
    uint8_t *body = http_read_body(req, &len, &err);
    if (!body) return http_error(req, err);
    char *p = strstr((char *)body, "\"bytecode_hex\"");
    p = p ? strchr(p + 14, '"') : NULL;
    if (!p) { free(body); return http_error(req, "missing bytecode_hex"); }
    p++;
    /* Decode hex in place: output never overtakes input */
    size_t n = 0;
    while (p[0] && p[1] && p[0] != '"') {
        int hi = hex_val(p[0]), lo = hex_val(p[1]);
        if (hi < 0 || lo < 0) break;
        body[n++] = (uint8_t)((hi << 4) | lo);
        p += 2;
    }
    esp_err_t r = n ? http_run(req, JOB_EXEC, body, n, NULL) : http_error(req, "invalid bytecode_hex");
    free(body);
    return r;
}

/* In-place percent-decoding of a query value ("%2Fspiffs%2Fapp.msbc") */
static void url_decode(char *s) {
    char *out = s;
    for (; *s; s++) {
        if (*s == '%' && hex_val(s[1]) >= 0 && hex_val(s[2]) >= 0) {
            *out++ = (char)((hex_val(s[1]) << 4) | hex_val(s[2]));
            s += 2;
        } else {
            *out++ = (*s == '+') ? ' ' : *s;
        }
    }
    *out = '\0';
}

/* Streams the body into SPIFFS: /api/upload?path=/spiffs/app.msbc&run=1.
 * Installing a boot app (app.msbc / app.fasl) with run=1 reboots into it. */
static esp_err_t http_upload(httpd_req_t *req) {
    char query[256] = {0}, path[64] = APP_MSBC_PATH, run[8] = "1";
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "path", path, sizeof(path));
        httpd_query_key_value(query, "run", run, sizeof(run));
        url_decode(path);
    }
    if (!safe_spiffs_path(path)) return http_error(req, "path must be under /spiffs/");
    bool save_only = strcmp(run, "0") == 0;
    if (req->content_len == 0 && !save_only) return http_error(req, "empty body"); /* nothing to run */

    FILE *fp = fopen(UPLOAD_TMP_PATH, "wb");
    if (!fp) return http_error(req, "cannot open upload file");
    char chunk[512];
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, chunk, sizeof(chunk));
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0 || fwrite(chunk, 1, (size_t)r, fp) != (size_t)r) {
            fclose(fp);
            remove(UPLOAD_TMP_PATH);
            return http_error(req, "upload failed");
        }
        got += (size_t)r;
    }
    fclose(fp);
    /* The body was staged in UPLOAD_TMP_PATH; an upload aimed there is done */
    if (strcmp(path, UPLOAD_TMP_PATH) != 0) {
        remove(path);
        if (rename(UPLOAD_TMP_PATH, path) != 0) return http_error(req, "rename failed");
    }
    ESP_LOGI(TAG, "Uploaded %u bytes to %s", (unsigned)got, path);
    bool app = is_app_path(path);
    if (app) app_installed(path);
    if (strcmp(run, "0") == 0 || app) {
        bool restart = app && strcmp(run, "0") != 0;
        char out[160];
        snprintf(out, sizeof(out), "{\"success\":true,\"saved\":\"%s\",\"size\":%u,\"restart\":%s}",
                 path, (unsigned)got, restart ? "true" : "false");
        esp_err_t r = http_send_json(req, out);
        if (restart) restart_into_app();
        return r;
    }
    return http_run(req, JOB_RUN_FILE, NULL, 0, path);
}

/* A percent-decoded query parameter; false when missing */
static bool http_query(httpd_req_t *req, const char *key, char *out, size_t cap) {
    char query[256];
    out[0] = '\0';
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) return false;
    if (httpd_query_key_value(query, key, out, cap) != ESP_OK) return false;
    url_decode(out);
    return true;
}

static void http_chunk_sink(void *ctx, const char *text) {
    httpd_resp_sendstr_chunk((httpd_req_t *)ctx, text);
}

static esp_err_t http_fs_list(httpd_req_t *req) {
    http_cors(req);
    httpd_resp_set_type(req, "application/json");
    fs_list_json(http_chunk_sink, req);
    return httpd_resp_sendstr_chunk(req, NULL);
}

/* GET /api/fs/read?path=/spiffs/config.ini: the raw file (errors as JSON) */
static esp_err_t http_fs_read(httpd_req_t *req) {
    char path[64];
    http_query(req, "path", path, sizeof(path));
    if (!safe_spiffs_path(path)) return http_error(req, "path must be under /spiffs/");
    FILE *fp = fopen(path, "rb");
    if (!fp) return http_error(req, "no such file");
    http_cors(req);
    httpd_resp_set_type(req, "application/octet-stream");
    char chunk[512];
    size_t n;
    esp_err_t r = ESP_OK;
    while (r == ESP_OK && (n = fread(chunk, 1, sizeof(chunk), fp)) > 0) r = httpd_resp_send_chunk(req, chunk, n);
    fclose(fp);
    return r == ESP_OK ? httpd_resp_send_chunk(req, NULL, 0) : r;
}

static esp_err_t http_fs_delete(httpd_req_t *req) {
    char path[64];
    http_query(req, "path", path, sizeof(path));
    const char *err = fs_remove(path);
    return err ? http_error(req, err) : http_send_json(req, "{\"success\":true}");
}

static esp_err_t http_fs_rename(httpd_req_t *req) {
    char from[64], to[64];
    http_query(req, "from", from, sizeof(from));
    http_query(req, "to", to, sizeof(to));
    const char *err = fs_rename(from, to);
    return err ? http_error(req, err) : http_send_json(req, "{\"success\":true}");
}

/* POST /api/reset: reboot, e.g. after editing config.ini */
/* POST /api/display?color_bits=4&palette=amber: applied at once and saved
 * to config.ini [display] (either parameter may be left out) */
static esp_err_t http_display(httpd_req_t *req) {
    char v[16];
    RemoteJob job = { .kind = JOB_DISPLAY, .scheme = -1 };
    if (http_query(req, "color_bits", v, sizeof(v))) {
        job.bits = atoi(v);
        if (job.bits != 1 && job.bits != 2 && job.bits != 4 && job.bits != 8 && job.bits != 16) {
            return http_error(req, "color_bits: 16, 8, 4, 2 or 1");
        }
    }
    if (http_query(req, "palette", v, sizeof(v))) {
        job.scheme = mini_gui_scheme_from_name(v);
        if (job.scheme < 0) return http_error(req, "palette: color, gray, eink, green, amber, cyan, gameboy or invert");
    }
    if (!job.bits && job.scheme < 0) return http_error(req, "expected color_bits and/or palette");
    char out[256];
    job.out = out;
    job.out_cap = sizeof(out);
    submit_job_and_wait(&job);
    return http_send_json(req, out);
}

/* POST /api/input?button=1|0 and/or light=<0..4095>|off: the WebREPL's KEY 18
 * (renewed with button=1 while held) and simulated light sensor */
static esp_err_t http_input(httpd_req_t *req) {
    char v[12];
    if (http_query(req, "button", v, sizeof(v))) {
        if (v[0] == '1') {
            if (!s_remote_btn || esp_timer_get_time() >= s_remote_btn_until) s_remote_presses++;
            s_remote_btn_until = esp_timer_get_time() + REMOTE_BUTTON_HOLD_US;
            s_remote_btn = true;
        } else {
            s_remote_btn = false;
        }
    }
    if (http_query(req, "light", v, sizeof(v))) {
        if (strcmp(v, "off") == 0) {
            s_light_override = -1;
        } else {
            long n = strtol(v, NULL, 10);
            s_light_override = n < 0 ? 0 : (n > 4095 ? 4095 : (int)n);
        }
        s_light_dirty = true;
    }
    char out[96];
    snprintf(out, sizeof(out), "{\"success\":true,\"button\":%s,\"light_remote\":%s}",
             s_remote_btn ? "true" : "false", s_light_override >= 0 ? "true" : "false");
    return http_send_json(req, out);
}

static volatile int64_t s_settings_restart_at;

/* The market's view of the system settings built into the firmware */
static const uint8_t *builtin_app(const char *id, size_t *len) {
    if (strcmp(id, "settings") != 0) return NULL;
    *len = APP_SETTINGS_MSBC_LEN;
    return app_settings_msbc_data;
}

/* The boot check found updates (minischeme_market.c): the settings install
 * them a second later and return to the app */
static void market_auto_update(void) {
    s_market_auto = MARKET_AUTO_UPDATE_MAGIC;
    s_settings_restart_at = esp_timer_get_time() + 1000000;
}

/* POST /api/reset: restart into the app; ?settings=1: into the system
 * settings (installing and updating apps happens there) */
static esp_err_t http_reset(httpd_req_t *req) {
    char settings[4] = "";
    http_query(req, "settings", settings, sizeof(settings));
    esp_err_t r = http_send_json(req, "{\"success\":true}");
    if (settings[0] == '1' && !s_settings_mode) {
        s_settings_restart_at = esp_timer_get_time() + 1000000;   /* the main loop, after this answer */
        return r;
    }
    restart_into_app();
    return r;
}

/* GET /: the settings page (setup_page.html, gzip), in setup mode and on the LAN */
static esp_err_t http_root(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)setup_page_gz, SETUP_PAGE_GZ_LEN);
}

/* Any other GET: in setup mode a phone's connectivity check lands here (the
 * captive DNS answers every name with this board); send it to the page */
static esp_err_t http_captive(httpd_req_t *req) {
    if (!setup_portal_active()) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    char url[40];
    snprintf(url, sizeof(url), "http://%s/", setup_portal_ip());
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", url);
    return httpd_resp_send(req, NULL, 0);
}

/* A small text file, malloc'd and NUL-terminated ("" when missing) */
static char *read_small_file(const char *path, size_t max) {
    FILE *fp = fopen(path, "rb");
    long size = 0;
    if (fp) {
        fseek(fp, 0, SEEK_END);
        size = ftell(fp);
        fseek(fp, 0, SEEK_SET);
    }
    if (size < 0 || (size_t)size > max) size = 0;
    char *text = (char *)malloc((size_t)size + 1);
    if (!text) {
        if (fp) fclose(fp);
        return NULL;
    }
    size_t got = fp ? fread(text, 1, (size_t)size, fp) : 0;
    if (fp) fclose(fp);
    text[got] = '\0';
    return text;
}

/* The station's link: connected, network, address, signal */
static bool sta_status(char *ssid, size_t ssid_cap, char *ip, size_t ip_cap, int *rssi) {
    ssid[0] = '\0';
    snprintf(ip, ip_cap, "0.0.0.0");
    *rssi = 0;
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info;
    if (!sta || esp_netif_get_ip_info(sta, &info) != ESP_OK || info.ip.addr == 0) return false;
    esp_ip4addr_ntoa(&info.ip, ip, (int)ip_cap);
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        snprintf(ssid, ssid_cap, "%s", (const char *)ap.ssid);
        *rssi = ap.rssi;
    }
    return true;
}

/* GET /api/settings: what the settings page shows (never the password) */
static esp_err_t http_settings_get(httpd_req_t *req) {
    char *cfg = read_small_file("/spiffs/config.ini", 32 * 1024);
    char *out = (char *)malloc(2048);
    char *esc = (char *)malloc(200);
    if (!cfg || !out || !esc) {
        free(cfg); free(out); free(esc);
        return http_error(req, "out of memory");
    }
    static const char *const keys[] = { "device_name", "boot_app", "wifi_ssid", "proxy", "market_url",
                                        "market_auto_update", "web_in_app", "https_proxy", "factory_reset_key",
                                        "app_storage_kb", "button_long_ms", "button_mid_ms", "button_hold_ms",
                                        "channel_server", "channel_id", "proxy_via_channel", "market_via_channel" };
    enum { NKEYS = sizeof(keys) / sizeof(keys[0]) };
    char vals[NKEYS][96];
    for (int i = 0; i < NKEYS; i++) {
        if (!mini_device_config_value(cfg, keys[i], vals[i], sizeof(vals[i]))) vals[i][0] = '\0';
    }
    char key_hex[72] = "";   /* never sent: whether one is set */
    bool key_set = mini_device_config_value(cfg, "channel_key", key_hex, sizeof(key_hex)) && key_hex[0];
    mini_wipe(key_hex, sizeof(key_hex));
    free(cfg);
    char ssid[33], ip[16];
    int rssi;
    bool sta = sta_status(ssid, sizeof(ssid), ip, sizeof(ip), &rssi);
    size_t n = (size_t)snprintf(out, 2048, "{\"success\":true,\"settings_mode\":%s,\"saved\":%d,\"free_heap\":%u",
                                s_settings_mode ? "true" : "false", s_settings_saved,
                                (unsigned)esp_get_free_heap_size());
    static const char *const names[] = { "device_name", "boot", "wifi_ssid", "proxy", "market_url",
                                         "market_auto_update", "web_in_app", "https_proxy", "factory_reset_key",
                                         "app_storage_kb", "button_long_ms", "button_mid_ms", "button_hold_ms",
                                         "channel_server", "channel_id", "proxy_via_channel", "market_via_channel" };
    n += (size_t)snprintf(out + n, 2048 - n, ",\"channel_key_set\":%s,\"channel_ready\":%s",
                          key_set ? "true" : "false", mini_chan_enabled() ? "true" : "false");
    for (int i = 0; i < NKEYS && n < 1900; i++) {
        json_escape(vals[i], esc, 200);
        n += (size_t)snprintf(out + n, 2048 - n, ",\"%s\":\"%s\"", names[i], esc);
    }
    json_escape(s_settings_mode ? "settings" : s_app_path, esc, 200);
    n += (size_t)snprintf(out + n, 2048 - n, ",\"app\":\"%s\",\"settings_from\":\"%s\"", esc,
                          s_settings_mode ? s_settings_from : "");
    json_escape(s_settings_mode ? "" : (s_app_name[0] ? s_app_name : s_app_id), esc, 200);
    n += (size_t)snprintf(out + n, 2048 - n, ",\"app_name\":\"%s\",\"app_web\":%s", esc,
                          (!s_settings_mode && s_app_web_len) ? "true" : "false");
    json_escape(ssid, esc, 200);
    n += (size_t)snprintf(out + n, 2048 - n, ",\"sta_connected\":%s,\"sta_ssid\":\"%s\",\"sta_ip\":\"%s\",\"rssi\":%d",
                          sta ? "true" : "false", esc, ip, rssi);
    uint32_t bl, bm, bh;
    mini_device_button_get_timing(&bl, &bm, &bh);   /* the times in effect */
    n += (size_t)snprintf(out + n, 2048 - n, ",\"button_ms\":[%u,%u,%u]", (unsigned)bl, (unsigned)bm, (unsigned)bh);
    json_escape(setup_portal_ssid(), esc, 200);
    snprintf(out + n, 2048 - n, ",\"ap\":%s,\"ap_ssid\":\"%s\",\"ap_ip\":\"%s\",\"clients\":%d}",
             setup_portal_active() ? "true" : "false", esc, setup_portal_ip(), setup_portal_clients());
    esp_err_t r = http_send_json(req, out);
    free(out);
    free(esc);
    return r;
}

static bool settings_name_ok(const char *s) {
    if (!*s) return false;
    for (; *s; s++) {
        if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '_')) return false;
    }
    return true;
}

/* POST /api/settings, body "section.key=value&...": each value written to
 * config.ini (comments kept); new WiFi settings are tried at once */
static esp_err_t http_settings_post(httpd_req_t *req) {
    const char *err = NULL;
    size_t len = 0;
    if (req->content_len > 2048) return http_error(req, "body too large");
    char *body = (char *)http_read_body(req, &len, &err);
    if (!body) return http_error(req, err);
    char wifi_ssid[33] = "", wifi_pass[65] = "";
    bool wifi = false, have_pass = false, buttons = false;
    int saved = 0;
    for (char *field = strtok(body, "&"); field; field = strtok(NULL, "&")) {
        char *eq = strchr(field, '=');
        if (!eq) continue;
        *eq = '\0';
        char *value = eq + 1;
        url_decode(field);
        url_decode(value);
        char *dot = strchr(field, '.');
        if (!dot || strchr(value, '\n') || strchr(value, '\r') || strlen(value) > 120) {
            free(body);
            return http_error(req, "expected section.key=value (one line, up to 120 bytes)");
        }
        *dot = '\0';
        const char *section = field, *key = dot + 1;
        if (!settings_name_ok(section) || !settings_name_ok(key)) {
            free(body);
            return http_error(req, "section and key: a-z, 0-9, _");
        }
        const char *v = value;
        if (!config_save(section, &key, &v, 1)) {
            free(body);
            return http_error(req, "cannot write /spiffs/config.ini");
        }
        saved++;
        if (strcmp(key, "wifi_ssid") == 0) { snprintf(wifi_ssid, sizeof(wifi_ssid), "%s", value); wifi = true; }
        if (strcmp(key, "wifi_password") == 0) { snprintf(wifi_pass, sizeof(wifi_pass), "%s", value); have_pass = true; wifi = true; }
        if (strncmp(key, "button_", 7) == 0) buttons = true;
        ESP_LOGI(TAG, "Settings page: [%s] %s set", section, key);
    }
    free(body);
    if (buttons) {   /* the KEY 18 times apply at once */
        char *cfg = read_small_file("/spiffs/config.ini", 32 * 1024);
        if (cfg) {
            mini_device_button_config(cfg);
            free(cfg);
        }
    }
    if (wifi) {
        /* a value left out (the password field kept empty) comes from config.ini */
        char *cfg = read_small_file("/spiffs/config.ini", 32 * 1024);
        if (cfg) {
            if (!wifi_ssid[0]) mini_device_config_value(cfg, "wifi_ssid", wifi_ssid, sizeof(wifi_ssid));
            if (!have_pass) mini_device_config_value(cfg, "wifi_password", wifi_pass, sizeof(wifi_pass));
            free(cfg);
        }
        if (wifi_ssid[0]) {
            ESP_LOGI(TAG, "Settings page: connecting to %s", wifi_ssid);
            hal_wifi_connect(wifi_ssid, wifi_pass);
        }
    }
    s_settings_saved++;
    char out[96];
    snprintf(out, sizeof(out), "{\"success\":true,\"saved\":%d,\"wifi\":%s}", saved, wifi ? "true" : "false");
    return http_send_json(req, out);
}

/* GET /api/apps (apps_json_stream): every installed app with its whole
 * metadata ([fields] drive the settings forms) */
static esp_err_t http_apps(httpd_req_t *req) {
    http_cors(req);
    httpd_resp_set_type(req, "application/json");
    char meta[4] = "";
    http_query(req, "meta", meta, sizeof(meta));
    apps_json_stream(http_chunk_sink, req, meta[0] == '1');
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* GET /api/hardware (job_reply_hardware) */
static esp_err_t http_hardware(httpd_req_t *req) {
    char *out = (char *)malloc(4096);
    if (!out) return http_error(req, "out of memory");
    out[0] = '\0';
    RemoteJob job = { .kind = JOB_HARDWARE, .out = out, .out_cap = 4096 };
    submit_job_and_wait(&job);
    esp_err_t r = http_send_json(req, out);
    free(out);
    return r;
}

/* GET /api/market: the market's state and apps; POST /api/market?action=
 * refresh | install&id=<id>|updates | cancel, answered with the same JSON */
static esp_err_t http_market(httpd_req_t *req) {
    char action[16] = "", id[32] = "";
    RemoteJob job = { .kind = JOB_MARKET, .out_cap = 6144 };
    if (req->method == HTTP_POST) {
        http_query(req, "action", action, sizeof(action));
        http_query(req, "id", id, sizeof(id));
        if (strcmp(action, "install") == 0) snprintf(job.path, sizeof(job.path), "install:%s", id[0] ? id : "updates");
        else if (strcmp(action, "refresh") == 0 || strcmp(action, "cancel") == 0) snprintf(job.path, sizeof(job.path), "%s", action);
        else return http_error(req, "action: refresh, install or cancel");
    } else {
        snprintf(job.path, sizeof(job.path), "status");
    }
    job.out = (char *)malloc(job.out_cap);
    if (!job.out) return http_error(req, "out of memory");
    job.out[0] = '\0';
    submit_job_and_wait(&job);
    esp_err_t r = http_send_json(req, job.out);
    free(job.out);
    return r;
}

/* ---- Who may call the API -------------------------------------------------------
 * Every /api route goes through http_guard:
 *  - a browser page of another origin may call it only from this network
 *    (localhost, 127.x, 10.x, 172.16-31.x, 192.168.x, *.local): a page on the
 *    internet cannot reach the board through the visitor's browser;
 *  - "Origin: null" is an app's own page (sandboxed by /app's CSP): it may
 *    call /api/app/call, its app's hook, and nothing else;
 *  - the board itself (an app's JSON feed aimed at its own address) gets
 *    nothing: the API is not for the apps. */
static bool private_host(const char *h, size_t n) {
    char host[64];
    if (n == 0 || n >= sizeof(host)) return false;
    memcpy(host, h, n);
    host[n] = '\0';
    if (strcmp(host, "localhost") == 0 || (n > 6 && strcmp(host + n - 6, ".local") == 0)) return true;
    unsigned a, b, c, d;
    if (sscanf(host, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return false;
    return a == 127 || a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168);
}

static bool origin_allowed(const char *origin) {
    const char *h = strstr(origin, "://");
    if (!h) return false;
    h += 3;
    size_t n = 0;
    while (h[n] && h[n] != ':' && h[n] != '/') n++;
    return private_host(h, n);
}

/* The request comes from this board (127.0.0.1 or one of its addresses) */
static bool from_this_board(httpd_req_t *req) {
    struct sockaddr_storage peer;
    socklen_t len = sizeof(peer);
    if (getpeername(httpd_req_to_sockfd(req), (struct sockaddr *)&peer, &len) != 0) return false;
    uint32_t ip = 0;
    if (peer.ss_family == AF_INET) {
        ip = ((struct sockaddr_in *)&peer)->sin_addr.s_addr;
    } else if (peer.ss_family == AF_INET6) {
        const uint8_t *a = ((struct sockaddr_in6 *)&peer)->sin6_addr.s6_addr;
        static const uint8_t v4mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };
        if (memcmp(a, v4mapped, 12) != 0) return false;
        memcpy(&ip, a + 12, 4);
    }
    if ((ntohl(ip) >> 24) == 127) return true;
    const char *ifs[2] = { "WIFI_STA_DEF", "WIFI_AP_DEF" };
    for (int i = 0; i < 2; i++) {
        esp_netif_t *nif = esp_netif_get_handle_from_ifkey(ifs[i]);
        esp_netif_ip_info_t info;
        if (nif && esp_netif_get_ip_info(nif, &info) == ESP_OK && info.ip.addr == ip && ip) return true;
    }
    return false;
}

typedef esp_err_t (*HttpHandler)(httpd_req_t *req);
static char s_http_password[65] = "";

static esp_err_t http_guard(httpd_req_t *req) {
    bool app_call = strncmp(req->uri, "/api/app/call", 13) == 0 || strncmp(req->uri, "/api/app/file", 13) == 0;
    char origin[96];
    const char *why = NULL;
    if (httpd_req_get_hdr_value_len(req, "Origin") > 0) {
        if (httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) != ESP_OK) {
            why = "origin not allowed";
        } else if (strcmp(origin, "null") == 0 ? !app_call : !origin_allowed(origin)) {
            why = "origin not allowed (an app's page may only call /api/app/call and /api/app/file)";
        }
    }
    if (!why && from_this_board(req)) why = "not for requests from the board itself";
    if (why) {
        ESP_LOGW(TAG, "Refused %s: %s", req->uri, why);
        httpd_resp_set_status(req, "403 Forbidden");
        http_cors(req);
        httpd_resp_set_type(req, "application/json");
        char out[160];
        snprintf(out, sizeof(out), "{\"success\":false,\"error\":\"%s\"}", why);
        return httpd_resp_sendstr(req, out);
    }
    /* Password protection for HTTP API if configured */
    if (s_http_password[0] != '\0' && !app_call) {
        char auth_buf[96] = "";
        bool authed = false;
        if (httpd_req_get_hdr_value_len(req, "X-MiniScheme-Auth") > 0) {
            if (httpd_req_get_hdr_value_str(req, "X-MiniScheme-Auth", auth_buf, sizeof(auth_buf)) == ESP_OK) {
                if (strcmp(auth_buf, s_http_password) == 0) authed = true;
            }
        }
        if (!authed && httpd_req_get_url_query_len(req) > 0) {
            char query[128];
            if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
                if (httpd_query_key_value(query, "token", auth_buf, sizeof(auth_buf)) == ESP_OK ||
                    httpd_query_key_value(query, "auth", auth_buf, sizeof(auth_buf)) == ESP_OK) {
                    if (strcmp(auth_buf, s_http_password) == 0) authed = true;
                }
            }
        }
        if (!authed) {
            ESP_LOGW(TAG, "Unauthorized access to %s (password required)", req->uri);
            httpd_resp_set_status(req, "401 Unauthorized");
            http_cors(req);
            httpd_resp_set_type(req, "application/json");
            return httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"unauthorized\",\"auth_required\":true}");
        }
    }
    return ((HttpHandler)req->user_ctx)(req);
}

/* /api/app/file?name=<file>[&delete=1]: the running app's own files for its
 * web page (with /api/app/call the only routes an app page may call): GET
 * reads one, POST writes the body (streamed to a temporary file, within the
 * app's storage quota), POST with delete=1 removes it. A name is relative
 * to the app's folder, one folder level at most; its image and config.ini
 * stay out of reach. */
#define APP_FILE_MAX (512 * 1024)

static size_t dir_used_bytes(const char *dir);

static size_t s_app_quota = 256 * 1024;   /* the running app's storage (main) */
static int s_app_storage_kb = 0;          /* its meta's [app] storage_kb, 0: none */

static bool app_file_name_ok(const char *n) {
    size_t len = strlen(n);
    if (len == 0 || len > 64 || n[0] == '/' || n[len - 1] == '/' || strstr(n, "..") || strstr(n, "//")) return false;
    int slashes = 0;
    for (const char *p = n; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '/') {
            if (++slashes > 1) return false;
        } else if (!isalnum(c) && c != '_' && c != '-' && c != '.') {
            return false;
        }
    }
    return strcmp(n, "app.msbc") != 0 && strcmp(n, "app.fasl") != 0 && strcmp(n, "config.ini") != 0;
}

static const char *app_file_type(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return "application/octet-stream";
    if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0) return "image/jpeg";
    if (strcasecmp(dot, ".png") == 0) return "image/png";
    if (strcasecmp(dot, ".gif") == 0) return "image/gif";
    if (strcasecmp(dot, ".json") == 0) return "application/json";
    if (strcasecmp(dot, ".txt") == 0) return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

static void app_files_changed(void) {
    RemoteJob job = { .kind = JOB_APP_FILES };
    submit_job_and_wait(&job);
}

static esp_err_t http_app_file(httpd_req_t *req) {
    if (s_settings_mode || !s_app_dir[0]) return http_error(req, "no app is running");
    char name[72] = "", del[4] = "";
    if (!http_query(req, "name", name, sizeof(name)) || !app_file_name_ok(name)) {
        return http_error(req, "name: a file of the app's folder (letters, digits, _ - ., one folder level)");
    }
    http_query(req, "delete", del, sizeof(del));
    char path[160];
    snprintf(path, sizeof(path), "%s/%s", s_app_dir, name);
    if (req->method == HTTP_GET) {
        FILE *fp = fopen(path, "rb");
        if (!fp) {
            httpd_resp_set_status(req, "404 Not Found");
            return http_error(req, "no such file");
        }
        char *buf = (char *)malloc(1024);
        if (!buf) {
            fclose(fp);
            return http_error(req, "out of memory");
        }
        http_cors(req);
        httpd_resp_set_type(req, app_file_type(name));
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        size_t n;
        esp_err_t r = ESP_OK;
        while (r == ESP_OK && (n = fread(buf, 1, 1024, fp)) > 0) r = httpd_resp_send_chunk(req, buf, (ssize_t)n);
        fclose(fp);
        free(buf);
        return r == ESP_OK ? httpd_resp_send_chunk(req, NULL, 0) : r;
    }
    if (del[0] == '1') {
        bool gone = remove(path) == 0;
        app_files_changed();
        return http_send_json(req, gone ? "{\"success\":true}" : "{\"success\":false,\"error\":\"no such file\"}");
    }
    size_t total = req->content_len;
    if (total == 0 || total > APP_FILE_MAX) return http_error(req, "a file of 1 byte to 512 KB");
    struct stat st;
    size_t old = stat(path, &st) == 0 ? (size_t)st.st_size : 0;
    size_t used = dir_used_bytes(s_app_dir);
    used = used > old ? used - old : 0;
    if (used + total > s_app_quota) {
        char out[128];
        snprintf(out, sizeof(out), "{\"success\":false,\"error\":\"the app's storage is full (%u of %u KB)\"}",
                 (unsigned)((used + 1023) / 1024), (unsigned)(s_app_quota / 1024));
        return http_send_json(req, out);
    }
    char *slash = strchr(name, '/');
    if (slash) {   /* its folder */
        char dir[160];
        snprintf(dir, sizeof(dir), "%s/%.*s", s_app_dir, (int)(slash - name), name);
        mkdir(dir, 0775);
    }
    char tmp[168];
    snprintf(tmp, sizeof(tmp), "%s.part", path);
    FILE *fp = fopen(tmp, "wb");
    char *buf = fp ? (char *)malloc(1024) : NULL;
    size_t got = 0;
    bool ok = buf != NULL;
    while (ok && got < total) {
        int r = httpd_req_recv(req, buf, total - got < 1024 ? total - got : 1024);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        ok = r > 0 && fwrite(buf, 1, (size_t)r, fp) == (size_t)r;
        got += r > 0 ? (size_t)r : 0;
    }
    free(buf);
    if (fp && fclose(fp) != 0) ok = false;
    if (!ok) {
        remove(tmp);
        return http_error(req, fp ? "write failed (storage full?)" : "cannot create the file");
    }
    remove(path);
    if (rename(tmp, path) != 0) {
        remove(tmp);
        return http_error(req, "cannot write the file");
    }
    app_files_changed();
    char out[128];
    snprintf(out, sizeof(out), "{\"success\":true,\"size\":%u}", (unsigned)total);
    return http_send_json(req, out);
}

/* GET /app: the running app's own web page (gzip'd in its image), streamed
 * from the file; the settings page shows it in its first tab. The CSP
 * sandbox gives the page an origin of its own ("null"): it cannot read the
 * settings page or call the board's API, only its app (/api/app/call). */
static esp_err_t http_app_page(httpd_req_t *req) {
    if (s_settings_mode || !s_app_web_len) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        return httpd_resp_sendstr(req, "<p style=\"font-family:sans-serif\">This app has no web page.</p>");
    }
    FILE *fp = fopen(s_app_path, "rb");
    uint8_t *buf = fp ? (uint8_t *)malloc(2048) : NULL;
    if (!fp || !buf || fseek(fp, s_app_web_at, SEEK_SET) != 0) {
        if (fp) fclose(fp);
        free(buf);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "cannot read the app's page");
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Content-Security-Policy", "sandbox allow-scripts allow-forms allow-modals allow-popups");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    size_t left = s_app_web_len;
    esp_err_t r = ESP_OK;
    while (left > 0 && r == ESP_OK) {
        size_t n = fread(buf, 1, left < 2048 ? left : 2048, fp);
        if (n == 0) break;
        r = httpd_resp_send_chunk(req, (const char *)buf, (ssize_t)n);
        left -= n;
    }
    fclose(fp);
    free(buf);
    if (r == ESP_OK) r = httpd_resp_send_chunk(req, NULL, 0);
    return r;
}

/* GET / POST /api/app/call?fn=<name>: the body (form fields, at most 4 KB)
 * goes to the app's (app-web name body); its string is the answer, sent as
 * JSON when it looks like JSON */
#define APP_CALL_BODY_MAX 4096
#define APP_CALL_OUT_MAX  8192
static esp_err_t http_app_call(httpd_req_t *req) {
    RemoteJob job = { .kind = JOB_APP_CALL, .out_cap = APP_CALL_OUT_MAX };
    if (!http_query(req, "fn", job.path, sizeof(job.path)) || !job.path[0]) return http_error(req, "fn: the call's name");
    uint8_t *body = NULL;
    if (req->method == HTTP_POST && req->content_len > 0) {
        if (req->content_len > APP_CALL_BODY_MAX) return http_error(req, "body too large (4 KB at most)");
        const char *err = NULL;
        size_t len = 0;
        body = http_read_body(req, &len, &err);
        if (!body) return http_error(req, err);
        job.buf = body;
        job.len = len;
    }
    job.out = (char *)malloc(job.out_cap);
    if (!job.out) {
        free(body);
        return http_error(req, "out of memory");
    }
    job.out[0] = '\0';
    submit_job_and_wait(&job);
    free(body);
    http_cors(req);
    const char *o = job.out;
    while (*o == ' ' || *o == '\n' || *o == '\r' || *o == '\t') o++;
    if (job.failed) httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_set_type(req, (*o == '{' || *o == '[') ? "application/json" : "text/plain; charset=utf-8");
    esp_err_t r = httpd_resp_send(req, job.out, (ssize_t)strlen(job.out));
    free(job.out);
    return r;
}

/* ---- Installed apps by key ("apps/<id>", "user/<id>", "builtin") ---------------- */

/* The app a key names: its folder and image; false when none is installed */
static bool app_by_key(const char *key, char *dir, size_t dcap, char *image, size_t icap) {
    if (strcmp(key, "builtin") == 0) {
        bool fasl = file_has_magic(APP_FASL_PATH, "MFAS");
        const char *root = fasl ? APP_FASL_PATH : APP_MSBC_PATH;
        if (!fasl && !file_has_magic(root, "MSBC")) return false;
        char *meta = mini_app_meta_read_file(root);
        char id[32];
        mini_app_id(root, meta, id, sizeof(id));
        free(meta);
        mini_app_dir_of(root, id, dir, dcap);
        snprintf(image, icap, "%s", root);
        return true;
    }
    if (!mini_app_key_dir(key, dir, dcap)) return false;
    snprintf(image, icap, "%s/app.msbc", dir);
    if (file_has_magic(image, "MSBC")) return true;
    snprintf(image, icap, "%s/app.fasl", dir);
    return file_has_magic(image, "MFAS");
}

/* A JSON string value of text (escaped), malloc'd with its quotes */
static char *json_string_alloc(const char *text) {
    size_t cap = strlen(text) * 6 + 3;     /* every byte may become \u00XX */
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    out[0] = '"';
    size_t n = 1 + json_escape(text, out + 1, cap - 2);
    out[n++] = '"';
    out[n] = '\0';
    return out;
}

/* GET /api/apps/config?key=: the app's metadata ([fields], [config]) and its
 * config.ini, read only when the page opens that app's form */
static esp_err_t http_app_config_get(httpd_req_t *req) {
    char key[48], dir[64], image[80], ini[96];
    if (!http_query(req, "key", key, sizeof(key)) || !app_by_key(key, dir, sizeof(dir), image, sizeof(image))) {
        return http_error(req, "no such app");
    }
    snprintf(ini, sizeof(ini), "%s/config.ini", dir);
    char *meta = mini_app_meta_read_file(image);
    FILE *fp = fopen(ini, "rb");
    bool exists = fp != NULL;
    if (fp) fclose(fp);
    char *text = exists ? read_small_file(ini, 16 * 1024) : NULL;
    char *jm = json_string_alloc(meta ? meta : ""), *jt = json_string_alloc(text ? text : ""), *jp = json_string_alloc(ini);
    free(meta);
    free(text);
    if (!jm || !jt || !jp) {
        free(jm); free(jt); free(jp);
        return http_error(req, "out of memory");
    }
    http_cors(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr_chunk(req, "{\"success\":true,\"path\":");
    httpd_resp_sendstr_chunk(req, jp);
    httpd_resp_sendstr_chunk(req, exists ? ",\"exists\":true,\"ini\":" : ",\"exists\":false,\"ini\":");
    httpd_resp_sendstr_chunk(req, jt);
    httpd_resp_sendstr_chunk(req, ",\"meta\":");
    httpd_resp_sendstr_chunk(req, jm);
    httpd_resp_sendstr_chunk(req, "}");
    free(jm); free(jt); free(jp);
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* POST /api/apps/config?key= (body: the whole config.ini) also while the app
 * runs: it reads its settings when it starts */
static esp_err_t http_app_config_post(httpd_req_t *req) {
    char key[48], dir[64], image[80], ini[96];
    if (!http_query(req, "key", key, sizeof(key)) || !app_by_key(key, dir, sizeof(dir), image, sizeof(image))) {
        return http_error(req, "no such app");
    }
    if (req->content_len > 16 * 1024) return http_error(req, "config too large (16 KB at most)");
    const char *err = NULL;
    size_t len = 0;
    uint8_t *body = req->content_len ? http_read_body(req, &len, &err) : (uint8_t *)calloc(1, 1);
    if (!body) return http_error(req, err ? err : "out of memory");
    snprintf(ini, sizeof(ini), "%s/config.ini", dir);
    FILE *fp = fopen(ini, "wb");
    bool ok = fp && fwrite(body, 1, len, fp) == len;
    if (fp) ok = fclose(fp) == 0 && ok;
    free(body);
    if (!ok) return http_error(req, "cannot write the config");
    ESP_LOGI(TAG, "Config of %s saved (%u bytes)", key, (unsigned)len);
    return http_send_json(req, "{\"success\":true}");
}

/* POST /api/apps/reset?key=: back to the defaults (config.ini goes; the app
 * writes it again from its metadata when it starts) */
static esp_err_t http_app_reset(httpd_req_t *req) {
    char key[48], dir[64], image[80], ini[96];
    if (!http_query(req, "key", key, sizeof(key)) || !app_by_key(key, dir, sizeof(dir), image, sizeof(image))) {
        return http_error(req, "no such app");
    }
    snprintf(ini, sizeof(ini), "%s/config.ini", dir);
    remove(ini);
    ESP_LOGI(TAG, "Config of %s reset", key);
    return http_send_json(req, "{\"success\":true}");
}

static int remove_tree(const char *dir);

/* POST /api/apps/remove?key= (system settings only): the app's folder goes,
 * its settings and data with it */
static esp_err_t http_app_remove(httpd_req_t *req) {
    char key[48], dir[64], image[80];
    if (!s_settings_mode) return http_error(req, "uninstall apps in the system settings");
    if (!http_query(req, "key", key, sizeof(key)) || strcmp(key, "builtin") == 0 ||
        !app_by_key(key, dir, sizeof(dir), image, sizeof(image))) {
        return http_error(req, "no such app (the built-in one stays)");
    }
    char boot[72];
    configured_boot_path(boot, sizeof(boot));   /* before the files go: it checks they exist */
    int n = remove_tree(dir);
    rmdir(dir);
    if (strncmp(boot, dir, strlen(dir)) == 0 && boot[strlen(dir)] == '/') {
        const char *k = "boot_app", *v = "";
        config_save("system", &k, &v, 1);   /* the built-in app boots instead */
    }
    ESP_LOGI(TAG, "Removed %s (%d files)", dir, n);
    return http_send_json(req, "{\"success\":true}");
}

/* POST /api/apps/upload (system settings only; body: an .msbc image): the
 * app is checked (image, metadata id, API) and goes to /spiffs/user/<id>/,
 * not managed by the market */
static esp_err_t http_app_upload(httpd_req_t *req) {
    if (!s_settings_mode) return http_error(req, "upload apps in the system settings");
    if (req->content_len < 16 || req->content_len > 512 * 1024) return http_error(req, "not an app image");
    FILE *fp = fopen(UPLOAD_TMP_PATH, "wb");
    if (!fp) return http_error(req, "cannot open upload file");
    char chunk[512];
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, chunk, sizeof(chunk));
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0 || fwrite(chunk, 1, (size_t)r, fp) != (size_t)r) {
            fclose(fp);
            remove(UPLOAD_TMP_PATH);
            return http_error(req, "upload failed");
        }
        got += (size_t)r;
    }
    fclose(fp);
    char why[128] = "", id[32] = "";
    char *meta = file_has_magic(UPLOAD_TMP_PATH, "MSBC") ? mini_app_meta_read_file(UPLOAD_TMP_PATH) : NULL;
    if (!file_has_magic(UPLOAD_TMP_PATH, "MSBC")) snprintf(why, sizeof(why), "not an .msbc app image");
    else if (!meta || !mini_device_config_value(meta, "id", id, sizeof(id)) || !mini_app_id_ok(id))
        snprintf(why, sizeof(why), "the image has no valid [app] id (letters, digits, _ and -)");
    else mini_api_check_meta(meta, why, sizeof(why));
    free(meta);
    if (why[0]) {
        remove(UPLOAD_TMP_PATH);
        return http_error(req, why);
    }
    char dir[64], path[96], other[96];
    snprintf(dir, sizeof(dir), USER_DIR "/%s", id);
    snprintf(path, sizeof(path), "%s/app.msbc", dir);
    snprintf(other, sizeof(other), "%s/app.fasl", dir);
    remove(other);
    remove(path);
    if (rename(UPLOAD_TMP_PATH, path) != 0) return http_error(req, "cannot install the app");
    ESP_LOGI(TAG, "User app %s installed (%u bytes)", path, (unsigned)got);
    char *one = app_json_alloc(path, "", false);
    if (!one) return http_send_json(req, "{\"success\":true}");
    /* {"key":...} -> {"success":true,"key":...} */
    size_t n = strlen(one);
    char *out = (char *)malloc(n + 24);
    if (out) snprintf(out, n + 24, "{\"success\":true,%s", one + 1);
    esp_err_t r = http_send_json(req, out ? out : "{\"success\":true}");
    free(out);
    free(one);
    return r;
}

/* GET /api/wifi/scan: the networks in reach, strongest first */
static esp_err_t http_wifi_scan(httpd_req_t *req) {
    wifi_scan_config_t sc = { 0 };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) return http_error(req, "scan failed (busy connecting?)");
    uint16_t count = 16;
    wifi_ap_record_t *aps = (wifi_ap_record_t *)calloc(count, sizeof(wifi_ap_record_t));
    char *out = (char *)malloc(1600);
    if (!aps || !out) {
        free(aps); free(out);
        esp_wifi_clear_ap_list();
        return http_error(req, "out of memory");
    }
    esp_wifi_scan_get_ap_records(&count, aps);
    size_t n = (size_t)snprintf(out, 1600, "{\"success\":true,\"networks\":[");
    char esc[80];
    int listed = 0;
    for (int i = 0; i < count && n < 1500; i++) {
        if (!aps[i].ssid[0]) continue;
        bool dup = false;
        for (int j = 0; j < i; j++) dup = dup || strcmp((const char *)aps[j].ssid, (const char *)aps[i].ssid) == 0;
        if (dup) continue;
        json_escape((const char *)aps[i].ssid, esc, sizeof(esc));
        n += (size_t)snprintf(out + n, 1600 - n, "%s{\"ssid\":\"%s\",\"rssi\":%d,\"secure\":%s}", listed ? "," : "",
                              esc, aps[i].rssi, aps[i].authmode == WIFI_AUTH_OPEN ? "false" : "true");
        listed++;
    }
    snprintf(out + n, 1600 - n, "]}");
    free(aps);
    esp_err_t r = http_send_json(req, out);
    free(out);
    return r;
}

/* config.ini [channel]: the encrypted converter for the app proxy and the
 * market (minischeme_channel.h); the market's base as the market computes it */
static bool config_on(const char *cfg, const char *key) {
    char v[8] = "";
    if (!cfg || !mini_device_config_value(cfg, key, v, sizeof(v))) return false;
    return v[0] == '1' || v[0] == 'y' || v[0] == 'Y' || v[0] == 't' || v[0] == 'T' ||
           ((v[0] == 'o' || v[0] == 'O') && (v[1] == 'n' || v[1] == 'N'));
}

static void setup_channel(const char *cfg) {
    char server[128] = "", id[72] = "", key[72] = "", proxy[128] = "", market[160] = "";
    if (cfg) {
        mini_device_config_value(cfg, "channel_server", server, sizeof(server));
        mini_device_config_value(cfg, "channel_id", id, sizeof(id));
        mini_device_config_value(cfg, "channel_key", key, sizeof(key));
        mini_device_config_value(cfg, "proxy", proxy, sizeof(proxy));
    }
    mini_market_url(market, sizeof(market));
    mini_chan_configure(server, id, key, proxy, config_on(cfg, "proxy_via_channel"),
                        market, config_on(cfg, "market_via_channel"));
    mini_wipe(key, sizeof(key));
    if (server[0]) {
        ESP_LOGI(TAG, "Channel %s: %s (app proxy %s, market %s)", server,
                 mini_chan_enabled() ? "ready" : "not set up (channel_id, a 64-digit channel_key)",
                 config_on(cfg, "proxy_via_channel") ? "through it" : "direct",
                 config_on(cfg, "market_via_channel") ? "through it" : "direct");
    }
}

static size_t s_http_stack = DEF_HTTP_STACK; /* [memory] http_stack */
static bool s_http_started = false;

static void start_http_server(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = s_http_stack;
    config.max_open_sockets = 3;
    config.max_uri_handlers = 40;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server failed to start");
        return;
    }
    const httpd_uri_t uris[] = {
        { .uri = "/",                    .method = HTTP_GET,     .handler = http_root },
        { .uri = "/api/info",            .method = HTTP_GET,     .handler = http_info },
        { .uri = "/api/stats",           .method = HTTP_GET,     .handler = http_stats },
        { .uri = "/api/heapmap",         .method = HTTP_GET,     .handler = http_heapmap },
        { .uri = "/api/screen",          .method = HTTP_GET,     .handler = http_screen },
        { .uri = "/api/exec",            .method = HTTP_POST,    .handler = http_exec_raw },
        { .uri = "/api/execute_bytecode",.method = HTTP_POST,    .handler = http_exec_hex },
        { .uri = "/api/eval",            .method = HTTP_POST,    .handler = http_eval },
        { .uri = "/api/upload",          .method = HTTP_POST,    .handler = http_upload },
        { .uri = "/api/reset",           .method = HTTP_POST,    .handler = http_reset },
        { .uri = "/api/display",         .method = HTTP_POST,    .handler = http_display },
        { .uri = "/api/input",           .method = HTTP_POST,    .handler = http_input },
        { .uri = "/api/fs/list",         .method = HTTP_GET,     .handler = http_fs_list },
        { .uri = "/api/fs/read",         .method = HTTP_GET,     .handler = http_fs_read },
        { .uri = "/api/fs/delete",       .method = HTTP_POST,    .handler = http_fs_delete },
        { .uri = "/api/fs/rename",       .method = HTTP_POST,    .handler = http_fs_rename },
        { .uri = "/api/settings",        .method = HTTP_GET,     .handler = http_settings_get },
        { .uri = "/api/settings",        .method = HTTP_POST,    .handler = http_settings_post },
        { .uri = "/api/apps",            .method = HTTP_GET,     .handler = http_apps },
        { .uri = "/api/hardware",        .method = HTTP_GET,     .handler = http_hardware },
        { .uri = "/api/market",          .method = HTTP_GET,     .handler = http_market },
        { .uri = "/api/market",          .method = HTTP_POST,    .handler = http_market },
        { .uri = "/api/wifi/scan",       .method = HTTP_GET,     .handler = http_wifi_scan },
        { .uri = "/app",                 .method = HTTP_GET,     .handler = http_app_page },
        { .uri = "/api/app/call",        .method = HTTP_GET,     .handler = http_app_call },
        { .uri = "/api/app/call",        .method = HTTP_POST,    .handler = http_app_call },
        { .uri = "/api/app/file",        .method = HTTP_GET,     .handler = http_app_file },
        { .uri = "/api/app/file",        .method = HTTP_POST,    .handler = http_app_file },
        { .uri = "/api/apps/config",     .method = HTTP_GET,     .handler = http_app_config_get },
        { .uri = "/api/apps/config",     .method = HTTP_POST,    .handler = http_app_config_post },
        { .uri = "/api/apps/reset",      .method = HTTP_POST,    .handler = http_app_reset },
        { .uri = "/api/apps/remove",     .method = HTTP_POST,    .handler = http_app_remove },
        { .uri = "/api/apps/upload",     .method = HTTP_POST,    .handler = http_app_upload },
        { .uri = "/api/*",               .method = HTTP_OPTIONS, .handler = http_options },
        { .uri = "/*",                   .method = HTTP_GET,     .handler = http_captive }, /* last */
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_uri_t u = uris[i];
        if (strncmp(u.uri, "/api/", 5) == 0 && u.method != HTTP_OPTIONS) {   /* who may call: http_guard */
            u.user_ctx = (void *)u.handler;
            u.handler = http_guard;
        }
        httpd_register_uri_handler(server, &u);
    }
    s_http_started = true;
    ESP_LOGI(TAG, "HTTP remote API listening on port %d", config.server_port);
}

#endif /* MINISCHEME_FW_HTTP */

/* ========================================================================= */
/* System Configuration (/spiffs/config.ini)                                 */
/* ========================================================================= */

typedef struct {
    /* [display] */
    MiniGuiMode mode;              /* band, full or direct */
    int band_lines;                /* rows per band */
    MiniGuiColorFormat color_fmt;  /* bus format: rgb565 or rgb666 */
    int color_bits;                /* bits per stored pixel: 16, 8, 4, 2 or 1 */
    MiniGuiScheme scheme;          /* color scheme (palette) */
    bool auto_backlight;           /* brightness from the light sensor */
    int backlight;                 /* fixed brightness otherwise */
    int width;
    int height;
    bool lcd_dma;                  /* I2S DMA bus (false: GPIO bit-bang) */
    int lcd_dma_lines;             /* rows per DMA transfer, 0 = whole band */
    uint32_t lcd_clock_hz;         /* requested WR clock */
    /* [memory] */
    size_t reserved_heap;          /* heap kept free for the system at run time */
    size_t max_heap;               /* upper limit of the VM heap */
    size_t vm_eden;                /* VM nursery */
    size_t vm_stack;               /* VM value stack slots */
    size_t vm_frames;              /* VM call frames */
    bool app_xip;                  /* run the app in place from Flash */
    size_t http_stack;             /* HTTP server task stack */
    /* [system] */
    char boot_app[64];             /* app to start, under /spiffs ("" = app.msbc / app.fasl) */
    char device_name[24];          /* settings hotspot and web page title */
    bool web_in_app;               /* the HTTP server while an app runs (always in the settings) */
    bool factory_reset_key;        /* KEY 18 held 10 s from power-on: erase the app ROM, built-in settings */
    int app_storage_kb;            /* an app folder's storage quota (its image included) */
    uint32_t button_ms[3];         /* KEY 18: the long, mid and hold times (0: the default) */
    /* [wifi] */
    char wifi_ssid[33];
    char wifi_pass[65];
    /* [http] */
    char http_password[65];
} SystemConfig;

static SystemConfig s_sys_cfg;

static void config_defaults(SystemConfig *cfg) {
    cfg->mode = MINI_GUI_MODE_BAND;
    cfg->band_lines = DEF_BAND_LINES;
    cfg->color_fmt = MINI_GUI_COLOR_RGB565;
    cfg->color_bits = 16;
    cfg->scheme = MINI_GUI_SCHEME_COLOR;
    cfg->auto_backlight = true;
    cfg->backlight = 220;
    cfg->width = 320;
    cfg->height = 480;
    cfg->lcd_dma = true;
    cfg->lcd_dma_lines = 0;
    cfg->lcd_clock_hz = DEF_LCD_CLOCK_HZ;
    cfg->reserved_heap = MIN_OS_RESERVE;
    cfg->max_heap = DEF_MAX_HEAP;
    cfg->vm_eden = DEF_VM_EDEN;
    cfg->vm_stack = DEF_VM_STACK;
    cfg->vm_frames = DEF_VM_FRAMES;
    cfg->app_xip = true;
    cfg->http_stack = DEF_HTTP_STACK;
    cfg->boot_app[0] = '\0';
    snprintf(cfg->device_name, sizeof(cfg->device_name), "%s", DEFAULT_DEVICE_NAME);
    cfg->web_in_app = true;
    cfg->factory_reset_key = true;
    cfg->app_storage_kb = 256;
    cfg->button_ms[0] = cfg->button_ms[1] = cfg->button_ms[2] = 0;
    cfg->wifi_ssid[0] = '\0';   /* no network until the settings page sets one */
    cfg->wifi_pass[0] = '\0';
    cfg->http_password[0] = '\0';
}


static bool parse_long(const char *val, long lo, long hi, long *out) {
    char *end = NULL;
    long v = strtol(val, &end, 0);
    if (end == val || v < lo || v > hi) return false;
    *out = v;
    return true;
}

/* Applies one key; returns false for an unknown key or an invalid value */
static bool config_apply(SystemConfig *cfg, const char *key, const char *val, int *version) {
    long v;
    if (strcasecmp(key, "config_version") == 0) {
        if (!parse_long(val, 0, 1000, &v)) return false;
        *version = (int)v;
    } else if (strcasecmp(key, "mode") == 0) {
        if (strcasecmp(val, "band") == 0 || strcmp(val, "2") == 0) cfg->mode = MINI_GUI_MODE_BAND;
        else if (strcasecmp(val, "direct") == 0 || strcmp(val, "1") == 0) cfg->mode = MINI_GUI_MODE_DIRECT;
        else if (strcasecmp(val, "full") == 0 || strcmp(val, "3") == 0) cfg->mode = MINI_GUI_MODE_FULL;
        else return false;
    } else if (strcasecmp(key, "band_lines") == 0) {
        if (!parse_long(val, 1, 120, &v)) return false;
        cfg->band_lines = (int)v;
    } else if (strcasecmp(key, "color_format") == 0) {
        if (strcasecmp(val, "rgb666") == 0 || strcmp(val, "18") == 0) cfg->color_fmt = MINI_GUI_COLOR_RGB666;
        else if (strcasecmp(val, "rgb565") == 0 || strcmp(val, "16") == 0) cfg->color_fmt = MINI_GUI_COLOR_RGB565;
        else if (strcasecmp(val, "4bpp") == 0 || strcmp(val, "4") == 0) cfg->color_bits = 4; /* legacy */
        else return false;
    } else if (strcasecmp(key, "color_bits") == 0) {
        if (!parse_long(val, 1, 16, &v) || (v != 1 && v != 2 && v != 4 && v != 8 && v != 16)) return false;
        cfg->color_bits = (int)v;
    } else if (strcasecmp(key, "palette") == 0) {
        int scheme = mini_gui_scheme_from_name(val);
        if (scheme < 0) return false;
        cfg->scheme = (MiniGuiScheme)scheme;
    } else if (strcasecmp(key, "auto_backlight") == 0) {
        if (!parse_long(val, 0, 1, &v)) return false;
        cfg->auto_backlight = v != 0;
    } else if (strcasecmp(key, "backlight") == 0) {
        if (!parse_long(val, 0, 255, &v)) return false;
        cfg->backlight = (int)v;
    } else if (strcasecmp(key, "width") == 0) {
        if (!parse_long(val, 16, 480, &v)) return false;
        cfg->width = (int)v;
    } else if (strcasecmp(key, "height") == 0) {
        if (!parse_long(val, 16, 480, &v)) return false;
        cfg->height = (int)v;
    } else if (strcasecmp(key, "lcd_dma") == 0) {
        if (!parse_long(val, 0, 1, &v)) return false;
        cfg->lcd_dma = v != 0;
    } else if (strcasecmp(key, "lcd_dma_lines") == 0) {
        if (!parse_long(val, 0, 120, &v)) return false;
        cfg->lcd_dma_lines = (int)v;
    } else if (strcasecmp(key, "lcd_clock_hz") == 0) {
        if (!parse_long(val, 1000000, 40000000, &v)) return false;
        cfg->lcd_clock_hz = (uint32_t)v;
    } else if (strcasecmp(key, "reserved_heap") == 0) {
        if (!parse_long(val, 16384, 1L << 20, &v)) return false;
        cfg->reserved_heap = (size_t)v;
    } else if (strcasecmp(key, "max_heap") == 0) {
        if (!parse_long(val, 16384, 1L << 22, &v)) return false;
        cfg->max_heap = (size_t)v;
    } else if (strcasecmp(key, "vm_eden") == 0) {
        if (!parse_long(val, 1024, 65536, &v)) return false;
        cfg->vm_eden = (size_t)v;
    } else if (strcasecmp(key, "vm_stack") == 0) {
        if (!parse_long(val, 128, 16384, &v)) return false;
        cfg->vm_stack = (size_t)v;
    } else if (strcasecmp(key, "vm_frames") == 0) {
        if (!parse_long(val, 16, 4096, &v)) return false;
        cfg->vm_frames = (size_t)v;
    } else if (strcasecmp(key, "app_xip") == 0) {
        if (!parse_long(val, 0, 1, &v)) return false;
        cfg->app_xip = v != 0;
    } else if (strcasecmp(key, "boot_app") == 0) {
        if (strlen(val) >= sizeof(cfg->boot_app)) return false;
        snprintf(cfg->boot_app, sizeof(cfg->boot_app), "%s", val);
    } else if (strcasecmp(key, "device_name") == 0) {
        if (!val[0] || strlen(val) >= sizeof(cfg->device_name)) return false;
        snprintf(cfg->device_name, sizeof(cfg->device_name), "%s", val);
    } else if (strcasecmp(key, "app_storage_kb") == 0) {
        if (!parse_long(val, 64, 1024, &v)) return false;
        cfg->app_storage_kb = (int)v;
    } else if (strcasecmp(key, "button_long_ms") == 0 || strcasecmp(key, "button_mid_ms") == 0 ||
               strcasecmp(key, "button_hold_ms") == 0) {
        /* empty: the default; the device layer keeps each time in its range */
        if (val[0] && !parse_long(val, 0, 60000, &v)) return false;
        int i = strcasecmp(key, "button_long_ms") == 0 ? 0 : (strcasecmp(key, "button_mid_ms") == 0 ? 1 : 2);
        cfg->button_ms[i] = val[0] ? (uint32_t)v : 0;
    } else if (strcasecmp(key, "factory_reset_key") == 0) {
        if (!parse_long(val, 0, 1, &v)) return false;
        cfg->factory_reset_key = v != 0;
    } else if (strcasecmp(key, "web_in_app") == 0) {
        if (!parse_long(val, 0, 1, &v)) return false;
        cfg->web_in_app = v != 0;
    } else if (strcasecmp(key, "wifi_ssid") == 0) {
        if (strlen(val) >= sizeof(cfg->wifi_ssid)) return false;
        snprintf(cfg->wifi_ssid, sizeof(cfg->wifi_ssid), "%s", val);
    } else if (strcasecmp(key, "wifi_password") == 0) {
        if (strlen(val) >= sizeof(cfg->wifi_pass)) return false;
        snprintf(cfg->wifi_pass, sizeof(cfg->wifi_pass), "%s", val);
    } else if (strcasecmp(key, "http_password") == 0 || strcasecmp(key, "password") == 0) {
        if (strlen(val) >= sizeof(cfg->http_password)) return false;
        snprintf(cfg->http_password, sizeof(cfg->http_password), "%s", val);
        snprintf(s_http_password, sizeof(s_http_password), "%s", val);
    } else if (strcasecmp(key, "proxy") == 0 || strcasecmp(key, "https_proxy") == 0 || strncasecmp(key, "app_", 4) == 0 ||
               strncasecmp(key, "pcmon_", 6) == 0) {
        /* [app] keys belong to Scheme apps: read with (config-get key) */
    } else if (strncasecmp(key, "channel_", 8) == 0 || strcasecmp(key, "proxy_via_channel") == 0 ||
               strcasecmp(key, "market_via_channel") == 0) {
        /* [channel]: applied by setup_channel at boot */
    } else if (strncasecmp(key, "font_", 5) == 0) {
        /* [fonts]: applied by apply_font_config once the font ROM is mapped */
    } else if (strcasecmp(key, "http_stack") == 0) {
        if (!parse_long(val, 3072, 32768, &v)) return false;
        cfg->http_stack = (size_t)v;
    } else {
        return false;
    }
    return true;
}

static bool write_config_template(const char *path) {
    FILE *fp = fopen(path, "w");
    if (!fp) return false;
    fputs(mini_device_config_template, fp);
    fclose(fp);
    return true;
}

/* Appends text to path when it has no `key` (sections added after config v3) */
static void config_append_missing(const char *path, const char *key, const char *text) {
    FILE *fp = fopen(path, "r");
    if (!fp) return;
    char line[192];
    bool found = false;
    size_t klen = strlen(key);
    while (!found && fgets(line, sizeof(line), fp)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        found = strncasecmp(p, key, klen) == 0 && (p[klen] == ' ' || p[klen] == '\t' || p[klen] == '=');
    }
    fclose(fp);
    if (found) return;
    fp = fopen(path, "a");
    if (!fp) return;
    fputs(text, fp);
    fclose(fp);
    ESP_LOGI(TAG, "%s: added the section of '%s'", path, key);
}

static void load_or_create_config(const char *path, SystemConfig *cfg) {
    config_defaults(cfg);
    FILE *fp = fopen(path, "r");
    if (!fp) {
        ESP_LOGI(TAG, "%s not found, writing the default configuration", path);
        if (!write_config_template(path)) ESP_LOGW(TAG, "Cannot write %s, using built-in defaults", path);
        return;
    }

    SystemConfig parsed;
    config_defaults(&parsed);
    int version = 0;
    char line[192];
    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        char *comment = strpbrk(p, ";#");
        if (comment) *comment = '\0';
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (*p == '\0' || *p == '[') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = p;
        char *val = eq + 1;
        char *k_end = key + strlen(key) - 1;
        while (k_end >= key && (*k_end == ' ' || *k_end == '\t')) { *k_end = '\0'; k_end--; }
        while (*val == ' ' || *val == '\t') val++;
        char *v_end = val + strlen(val) - 1;
        while (v_end >= val && (*v_end == ' ' || *v_end == '\t' || *v_end == '\r' || *v_end == '\n')) {
            *v_end = '\0';
            v_end--;
        }
        if (!config_apply(&parsed, key, val, &version)) {
            ESP_LOGW(TAG, "config.ini: ignoring '%s = %s'", key, val);
        }
    }
    fclose(fp);

    if (version < CONFIG_VERSION) {
        /* Files written by older firmware carry stale defaults (20-line RGB666
         * bands): replace them with the current template */
        ESP_LOGI(TAG, "Upgrading %s (version %d) to version %d defaults", path, version, CONFIG_VERSION);
        write_config_template(path);
    } else {
        *cfg = parsed;
        if (cfg->reserved_heap == 80 * 1024 || cfg->reserved_heap == 76 * 1024) {
            /* a former default (80 KB, 76 KB), never changed by hand: the current one, in the file too */
            char v[16];
            snprintf(v, sizeof(v), "%u", (unsigned)MIN_OS_RESERVE);
            const char *key = "reserved_heap", *val = v;
            cfg->reserved_heap = MIN_OS_RESERVE;
            if (config_save("memory", &key, &val, 1)) ESP_LOGI(TAG, "config.ini: reserved_heap %s (the new default)", v);
        }
    }
    mini_device_button_timing(cfg->button_ms[0], cfg->button_ms[1], cfg->button_ms[2]);
    ESP_LOGI(TAG, "Config: mode=%s band_lines=%d color_bits=%d palette=%s bus=%s %dx%d lcd_dma=%d dma_lines=%d clock=%u Hz",
             gui_mode_name(cfg->mode), cfg->band_lines, cfg->color_bits, mini_gui_scheme_name(cfg->scheme),
             cfg->color_fmt == MINI_GUI_COLOR_RGB666 ? "rgb666" : "rgb565",
             cfg->width, cfg->height, cfg->lcd_dma, cfg->lcd_dma_lines, (unsigned)cfg->lcd_clock_hz);
    ESP_LOGI(TAG, "Config: reserved_heap=%u max_heap=%u eden=%u stack=%u frames=%u xip=%d http_stack=%u",
             (unsigned)cfg->reserved_heap, (unsigned)cfg->max_heap, (unsigned)cfg->vm_eden, (unsigned)cfg->vm_stack,
             (unsigned)cfg->vm_frames, cfg->app_xip, (unsigned)cfg->http_stack);
}

/* ========================================================================= */
/* Display Service                                                           */
/* ========================================================================= */

static int64_t s_lcd_us = 0; /* time spent on the parallel bus per render */

static void lcd_emit_rows(const MiniGuiRows *rows, int y, int h, int width, void *userdata) {
    (void)userdata;
    mini_mirror_mark(y, h);
    int64_t t0 = esp_timer_get_time();
    esp32_ili9488_flush_rows(rows, width, y, h);
    s_lcd_us += esp_timer_get_time() - t0;
}

/* A moving text box on its own (mini_gui_set_rect_output): one window, one transfer */
static void lcd_emit_rect(const MiniGuiRows *rows, int x, int y, int w, int h, void *userdata) {
    (void)userdata;
    mini_mirror_mark(y, h);
    int64_t t0 = esp_timer_get_time();
    esp32_ili9488_flush_rows_at(rows, x, y, w, h);
    s_lcd_us += esp_timer_get_time() - t0;
}

static void lcd_direct_fill(int x, int y, int w, int h, uint16_t rgb565, void *userdata) {
    (void)userdata;
    mini_mirror_mark(y, h);
    int64_t t0 = esp_timer_get_time();
    esp32_ili9488_fill(x, y, w, h, rgb565);
    s_lcd_us += esp_timer_get_time() - t0;
}

static const MiniGuiDirectSink s_lcd_sink = { lcd_direct_fill };

/* Re-renders invalidated rows band by band and pushes them to the panel */
static void service_display(void);

static bool file_has_magic(const char *path, const char *magic);

/* key's value in an ini file into val; false when the file or key is missing */
static bool ini_file_value(const char *path, const char *key, char *val, size_t cap) {
    FILE *fp = path[0] ? fopen(path, "r") : NULL;
    if (!fp) return false;
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *text = size >= 0 && size < 32 * 1024 ? (char *)malloc((size_t)size + 1) : NULL;
    size_t got = text ? fread(text, 1, (size_t)size, fp) : 0;
    fclose(fp);
    if (!text) return false;
    text[got] = '\0';
    bool found = mini_device_config_value(text, key, val, cap);
    free(text);
    return found;
}

/* (config-get "key") => the value in the app's own config.ini (its folder),
 * else in /spiffs/config.ini, as a string; #f when neither has it. An app
 * gets no secrets of config.ini (Wi-Fi, *password*, ...: mini_app_config_key_ok). */
static Value prim_config_get(MiniVM *vm, int argc, Value *argv) {
    if (argc < 1 || !mini_is_string(argv[0])) {
        vm_error(vm, "config-get: expected a key string");
        return mini_void();
    }
    char val[160];
    const char *key = mini_string_data(argv[0]);
    if ((ini_file_value(s_app_ini, key, val, sizeof(val)) && val[0]) ||
        (mini_app_config_key_ok(key) && ini_file_value("/spiffs/config.ini", key, val, sizeof(val)))) {
        return mini_make_string(vm, val);
    }
    return mini_false();
}

/* (app-config-set! "key" "value") => #t once the app's own ini holds it */
static Value prim_app_config_set(MiniVM *vm, int argc, Value *argv) {
    if (argc < 2 || !mini_is_string(argv[0]) || !mini_is_string(argv[1])) {
        vm_error(vm, "app-config-set!: expected key and value strings");
        return mini_void();
    }
    FILE *fp = fopen(s_app_ini, "r");
    size_t len = 0;
    char *text = NULL;
    if (fp) {
        fseek(fp, 0, SEEK_END);
        long size = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        text = size >= 0 && size < 32 * 1024 ? (char *)malloc((size_t)size + 1) : NULL;
        len = text ? fread(text, 1, (size_t)size, fp) : 0;
        fclose(fp);
    }
    if (text) text[len] = '\0';
    char *next = mini_device_config_set(text ? text : "", "config", mini_string_data(argv[0]), mini_string_data(argv[1]));
    free(text);
    if (!next) return mini_false();
    fp = fopen(s_app_ini, "w");
    bool ok = fp && fputs(next, fp) >= 0;
    if (fp) ok = fclose(fp) == 0 && ok;
    free(next);
    return ok ? mini_true() : mini_false();
}

/* (app-info) => (path id ini dir) of the running app */
static Value prim_app_info(MiniVM *vm, int argc, Value *argv) {
    (void)argc; (void)argv;
    Value list = mini_nil(), tmp = mini_nil();
    mini_push_root(vm, &list);
    mini_push_root(vm, &tmp);
    const char *fields[4] = { s_app_dir, s_app_ini, s_app_id, s_app_path };
    for (int i = 0; i < 4; i++) {
        tmp = mini_make_string(vm, fields[i]);
        list = mini_make_cons(vm, tmp, list);
    }
    mini_pop_root(vm);
    mini_pop_root(vm);
    return list;
}

#define MAX_APPS 16

/* The installed apps: app.msbc / app.fasl in each folder of /spiffs/apps
 * (not the settings: no boot app) and of /spiffs/user, then the root
 * app.msbc / app.fasl unless the same app is installed in /spiffs/apps */
static int installed_apps(char (*paths)[64], int max) {
    static const char *const roots[2] = { APPS_DIR, USER_DIR };
    int n = 0;
    for (int r = 0; r < 2; r++) {
        DIR *dir = opendir(roots[r]);
        for (struct dirent *de = dir ? readdir(dir) : NULL; de && n < max; de = readdir(dir)) {
            if (!mini_app_id_ok(de->d_name) || (r == 0 && strcmp(de->d_name, "settings") == 0)) continue;
            if (snprintf(paths[n], 64, "%s/%.31s/app.msbc", roots[r], de->d_name) >= 64) continue;
            if (file_has_magic(paths[n], "MSBC")) { n++; continue; }
            snprintf(paths[n], 64, "%s/%.31s/app.fasl", roots[r], de->d_name);
            if (file_has_magic(paths[n], "MFAS")) n++;
        }
        if (dir) closedir(dir);
    }
    const char *root_apps[2] = { APP_MSBC_PATH, APP_FASL_PATH };
    for (int i = 0; i < 2 && n < max; i++) {
        if (!file_has_magic(root_apps[i], i ? "MFAS" : "MSBC") || root_app_shadowed(root_apps[i])) continue;
        snprintf(paths[n++], 64, "%s", root_apps[i]);
    }
    return n;
}

/* The app config.ini boots now: [system] boot_app, else app.msbc / app.fasl */
static void configured_boot_path(char *out, size_t cap) {
    char *cfg = read_small_file("/spiffs/config.ini", 32 * 1024);
    char boot[64] = "", path[72];
    if (cfg) mini_device_config_value(cfg, "boot_app", boot, sizeof(boot));
    free(cfg);
    const char *rel = strncmp(boot, "/spiffs/", 8) == 0 ? boot + 8 : (boot[0] == '/' ? boot + 1 : boot);
    snprintf(path, sizeof(path), "/spiffs/%s", rel);
    if (boot[0] && (file_has_magic(path, "MSBC") || file_has_magic(path, "MFAS"))) snprintf(out, cap, "%s", path);
    else snprintf(out, cap, "%s", file_has_magic(APP_FASL_PATH, "MFAS") ? APP_FASL_PATH : APP_MSBC_PATH);
}

/* One app's JSON (mini_app_json) into a malloc'd buffer, or NULL */
static char *app_json_alloc(const char *path, const char *boot, bool with_meta) {
    char *meta = mini_app_meta_read_file(path);
    struct stat st;
    long size = stat(path, &st) == 0 ? (long)st.st_size : 0;
    size_t cap = 768 + (meta ? strlen(meta) * 2 : 0);
    char *out = (char *)malloc(cap);
    int flags = (strcmp(path, boot) == 0 ? MINI_APP_BOOT : 0) |
                (!s_settings_mode && strcmp(path, s_app_path) == 0 ? MINI_APP_RUNNING : 0) |
                (mini_app_web_locate_file(path, NULL, NULL) ? MINI_APP_WEB : 0) | (with_meta ? MINI_APP_META : 0);
    if (out && !mini_app_json(out, cap, path, meta, size, flags)) {
        free(out);
        out = NULL;
    }
    free(meta);
    return out;
}

static void apps_json_stream(TextSink out, void *ctx, bool with_meta) {
    char (*paths)[64] = (char (*)[64])malloc(MAX_APPS * 64);
    char boot[72], head[160];
    if (!paths) {
        out(ctx, "{\"success\":false,\"error\":\"out of memory\"}");
        return;
    }
    configured_boot_path(boot, sizeof(boot));
    int n = installed_apps(paths, MAX_APPS);
    snprintf(head, sizeof(head), "{\"success\":true,\"boot\":\"%s\",\"apps\":[", boot);
    out(ctx, head);
    bool first = true;
    for (int i = 0; i < n; i++) {
        char *one = app_json_alloc(paths[i], boot, with_meta);
        if (!one) continue;
        if (!first) out(ctx, ",");
        out(ctx, one);
        first = false;
        free(one);
    }
    out(ctx, "]}");
    free(paths);
}

/* (last-app-stats) => ((id . "weather") (heap . n) (live . n) (peak . n) (rom . n)
 * (uptime-s . n) (free-heap . n) (min-free . n)) of the app the settings replaced, or #f */
static Value prim_last_app_stats(MiniVM *vm, int argc, Value *argv);

/* A boot app at /spiffs/app.msbc (or .fasl) that is also installed in
 * /spiffs/apps under its id: listed once, by the installed copy */
static bool root_app_shadowed(const char *path) {
    char *meta = mini_app_meta_read_file(path);
    char id[32], copy[64];
    mini_app_id(path, meta, id, sizeof(id));
    free(meta);
    snprintf(copy, sizeof(copy), APPS_DIR "/%.31s/app.msbc", id);
    if (file_has_magic(copy, "MSBC")) return true;
    snprintf(copy, sizeof(copy), APPS_DIR "/%.31s/app.fasl", id);
    return file_has_magic(copy, "MFAS");
}

/* (app-list) => ((path id name description pages version) ...): the apps in
 * /spiffs/apps, then /spiffs/app.msbc or app.fasl */
static Value prim_app_list(MiniVM *vm, int argc, Value *argv) {
    (void)argc; (void)argv;
    Value list = mini_nil(), entry = mini_nil();
    char (*paths)[64] = (char (*)[64])malloc(MAX_APPS * 64);
    if (!paths) return list;
    int n = installed_apps(paths, MAX_APPS);
    mini_push_root(vm, &list);
    mini_push_root(vm, &entry);
    for (int i = n - 1; i >= 0; i--) {
        char *meta = mini_app_meta_read_file(paths[i]);
        entry = mini_app_entry(vm, paths[i], meta);
        free(meta);
        list = mini_make_cons(vm, entry, list);
    }
    mini_pop_root(vm);
    mini_pop_root(vm);
    free(paths);
    return list;
}

/* (setup-mode-start) => (ssid ip) of the setup access point, or #f */
static Value prim_setup_mode_start(MiniVM *vm, int argc, Value *argv) {
    if (!mini_access_privileged(vm, "setup-mode-start")) return mini_void();
    (void)argc; (void)argv;
    if (!setup_portal_start()) return mini_false();
    Value list = mini_nil(), tmp = mini_nil();
    mini_push_root(vm, &list);
    mini_push_root(vm, &tmp);
    tmp = mini_make_string(vm, setup_portal_ip());
    list = mini_make_cons(vm, tmp, list);
    tmp = mini_make_string(vm, setup_portal_ssid());
    list = mini_make_cons(vm, tmp, list);
    mini_pop_root(vm);
    mini_pop_root(vm);
    return list;
}

static void alist_put(MiniVM *vm, Value *lst, const char *key, Value v) {
    mini_push_root(vm, &v);
    Value k = mini_make_symbol(vm, key);
    mini_push_root(vm, &k);
    Value cell = mini_make_cons(vm, k, v);
    mini_pop_root(vm);
    mini_pop_root(vm);
    mini_push_root(vm, &cell);
    *lst = mini_make_cons(vm, cell, *lst);
    mini_pop_root(vm);
}

/* (setup-mode-status) => ((ap . #t) (ap-ssid . "Desk-A1B2") (ap-ip . "192.168.4.1")
 *   (clients . 1) (sta-connected . #t) (sta-ssid . "...") (sta-ip . "...") (saved . 0)) */
static Value prim_setup_mode_status(MiniVM *vm, int argc, Value *argv) {
    (void)argc; (void)argv;
    char ssid[33], ip[16];
    int rssi;
    bool sta = sta_status(ssid, sizeof(ssid), ip, sizeof(ip), &rssi);
    Value lst = mini_nil();
    mini_push_root(vm, &lst);
    alist_put(vm, &lst, "saved", mini_make_fixnum(s_settings_saved));
    alist_put(vm, &lst, "sta-ip", mini_make_string(vm, ip));
    alist_put(vm, &lst, "sta-ssid", mini_make_string(vm, ssid));
    alist_put(vm, &lst, "sta-connected", mini_make_bool(sta));
    alist_put(vm, &lst, "clients", mini_make_fixnum(setup_portal_clients()));
    alist_put(vm, &lst, "ap-ip", mini_make_string(vm, setup_portal_ip()));
    alist_put(vm, &lst, "ap-ssid", mini_make_string(vm, setup_portal_ssid()));
    alist_put(vm, &lst, "ap", mini_make_bool(setup_portal_active()));
    mini_pop_root(vm);
    return lst;
}

static Value prim_last_app_stats(MiniVM *vm, int argc, Value *argv) {
    (void)argc; (void)argv;
    if (s_last_app.magic != LAST_APP_MAGIC) return mini_false();
    char id[33];
    snprintf(id, sizeof(id), "%.32s", s_last_app.id);
    Value lst = mini_nil();
    mini_push_root(vm, &lst);
    if (s_last_app.error[0]) {
        char phase[9], error[121];
        snprintf(phase, sizeof(phase), "%.8s", s_last_app.phase);
        snprintf(error, sizeof(error), "%.120s", s_last_app.error);
        alist_put(vm, &lst, "error", mini_make_string(vm, error));
        alist_put(vm, &lst, "phase", mini_make_symbol(vm, phase));
    }
    alist_put(vm, &lst, "min-free", mini_make_fixnum(s_last_app.min_free));
    alist_put(vm, &lst, "free-heap", mini_make_fixnum(s_last_app.free_heap));
    alist_put(vm, &lst, "uptime-s", mini_make_fixnum(s_last_app.uptime_s));
    alist_put(vm, &lst, "rom", mini_make_fixnum(s_last_app.rom));
    alist_put(vm, &lst, "peak", mini_make_fixnum(s_last_app.peak));
    alist_put(vm, &lst, "live", mini_make_fixnum(s_last_app.live));
    alist_put(vm, &lst, "heap", mini_make_fixnum(s_last_app.heap));
    alist_put(vm, &lst, "id", mini_make_string(vm, id));
    mini_pop_root(vm);
    return lst;
}

/* (fs-info) => (total used) bytes of the file system behind /spiffs */
static Value prim_fs_info(MiniVM *vm, int argc, Value *argv) {
    (void)argc; (void)argv;
    size_t total = 0, used = 0;
    lfsvfs_info(&total, &used);
    Value list = mini_make_cons(vm, mini_make_fixnum((int64_t)used), mini_nil());
    mini_push_root(vm, &list);
    list = mini_make_cons(vm, mini_make_fixnum((int64_t)total), list);
    mini_pop_root(vm);
    return list;
}

/* (fs-list "/spiffs/apps") => (("weather.msbc" . 32109) ...): the files of a directory */
static Value prim_fs_list(MiniVM *vm, int argc, Value *argv) {
    if (argc < 1 || !mini_is_string(argv[0])) {
        vm_error(vm, "fs-list: expected a directory path");
        return mini_void();
    }
    char dir_path[96];
    if (!mini_access_file(vm, "fs-list", mini_string_data(argv[0]), false, dir_path, sizeof(dir_path))) return mini_void();
    Value list = mini_nil(), name = mini_nil();
    mini_push_root(vm, &list);
    mini_push_root(vm, &name);
    DIR *dir = strncmp(dir_path, "/spiffs", 7) == 0 && !strstr(dir_path, "..") ? opendir(dir_path) : NULL;
    for (struct dirent *de = dir ? readdir(dir) : NULL; de; de = readdir(dir)) {
        char path[128];
        struct stat st;
        if (snprintf(path, sizeof(path), "%s/%s", dir_path, de->d_name) >= (int)sizeof(path)) continue;
        if (stat(path, &st) != 0 || S_ISDIR(st.st_mode)) continue;
        name = mini_make_string(vm, de->d_name);
        name = mini_make_cons(vm, name, mini_make_fixnum((int64_t)st.st_size));
        list = mini_make_cons(vm, name, list);
    }
    if (dir) closedir(dir);
    mini_pop_root(vm);
    mini_pop_root(vm);
    return list;
}

/* (config-set! "section" "key" "value") => #t once /spiffs/config.ini holds
 * it (the value replaced in place, comments kept); read again at the next boot */
static Value prim_config_set(MiniVM *vm, int argc, Value *argv) {
    if (!mini_access_privileged(vm, "config-set!")) return mini_void();
    if (argc < 3 || !mini_is_string(argv[0]) || !mini_is_string(argv[1]) || !mini_is_string(argv[2])) {
        vm_error(vm, "config-set!: expected section, key and value strings");
        return mini_void();
    }
    const char *key = mini_string_data(argv[1]), *value = mini_string_data(argv[2]);
    return config_save(mini_string_data(argv[0]), &key, &value, 1) ? mini_true() : mini_false();
}

/* ========================================================================= */
/* On-board benchmarks (tools/bench_board.py)                                */
/* ========================================================================= */

static void alist_add(MiniVM *vm, Value *lst, const char *key, int64_t v) {
    Value k = mini_make_symbol(vm, key);
    mini_push_root(vm, &k);
    Value cell = mini_make_cons(vm, k, mini_make_fixnum(v));
    mini_pop_root(vm);
    mini_push_root(vm, &cell);
    *lst = mini_make_cons(vm, cell, *lst);
    mini_pop_root(vm);
}

static int bench_count(int argc, Value *argv, int dflt) {
    int n = (argc > 0 && mini_is_fixnum(argv[0])) ? (int)mini_to_fixnum(argv[0]) : dflt;
    return n < 1 ? 1 : (n > 600 ? 600 : n);
}

/* (display-bench n [y h [replay?]]) => average render of the screen (or rows
 * y..y+h) through the real LCD path. Each pass runs the render procedure,
 * unless replay? is true: then it costs what a scrolling-text frame costs. */
static Value prim_display_bench(MiniVM *vm, int argc, Value *argv) {
    int n = bench_count(argc, argv, 10);
    int y0 = 0, y1 = mini_gui_get_height();
    if (argc >= 3 && mini_is_fixnum(argv[1]) && mini_is_fixnum(argv[2])) {
        y0 = (int)mini_to_fixnum(argv[1]);
        y1 = y0 + (int)mini_to_fixnum(argv[2]);
    }
    bool replay = argc >= 4 && mini_is_true(argv[3]);
    if (replay) { /* record once outside the timing */
        mini_gui_forget_recording();
        mini_gui_band_render(vm, y0, y1, lcd_emit_rows, NULL);
    }
    MiniStats s0, s1;
    mini_get_stats(vm, &s0);
    int64_t lcd = 0, t0 = esp_timer_get_time();
    for (int i = 0; i < n; i++) {
        s_lcd_us = 0;
        if (!replay) mini_gui_forget_recording();
        mini_gui_band_render(vm, y0, y1, lcd_emit_rows, NULL);
        lcd += s_lcd_us;
    }
    int64_t total = esp_timer_get_time() - t0;
    mini_get_stats(vm, &s1);
    Value lst = mini_nil();
    mini_push_root(vm, &lst);
    alist_add(vm, &lst, "alloc-bytes", (int64_t)((s1.alloc_bytes - s0.alloc_bytes) / (uint64_t)n));
    alist_add(vm, &lst, "lcd-us", lcd / n);
    alist_add(vm, &lst, "full-render-us", total / n);
    mini_pop_root(vm);
    return lst;
}

/* (tick-bench n) => average synthetic clock tick + partial redraw to the LCD */
static Value prim_tick_bench(MiniVM *vm, int argc, Value *argv) {
    int n = bench_count(argc, argv, 60);
    MiniStats s0, s1;
    mini_get_stats(vm, &s0);
    int64_t t0 = esp_timer_get_time();
    for (int i = 1; i <= n; i++) {
        MiniWallTime t;
        memset(&t, 0, sizeof(t));
        t.synced = true;
        t.year = 2026; t.month = 9; t.day = 29; t.wday = 2;
        t.hour = 14; t.minute = (i / 60) % 60; t.second = i % 60;
        mini_device_clock_tick(vm, &t, 1000 + i, 400 + (i % 50), 200);
        mini_clear_error(vm);
        int y0, y1;
        while (mini_gui_take_invalidated(&y0, &y1)) mini_gui_band_render(vm, y0, y1, lcd_emit_rows, NULL);
    }
    int64_t total = esp_timer_get_time() - t0;
    mini_get_stats(vm, &s1);
    mini_gui_invalidate(0, mini_gui_get_height()); /* repaint the real time next */
    Value lst = mini_nil();
    mini_push_root(vm, &lst);
    alist_add(vm, &lst, "minor-gcs", (int64_t)(s1.minor_gcs - s0.minor_gcs));
    alist_add(vm, &lst, "alloc-bytes", (int64_t)((s1.alloc_bytes - s0.alloc_bytes) / (uint64_t)n));
    alist_add(vm, &lst, "tick-us", total / n);
    mini_pop_root(vm);
    return lst;
}

/* (mem-report) => system heap figures and free stack bytes per task */
static Value prim_mem_report(MiniVM *vm, int argc, Value *argv) {
    (void)argc; (void)argv;
    static const char *const tasks[] = {
        "main", "httpd", "jsonfeed", "tiT", "wifi", "sys_evt", "esp_timer", "ipc0", "ipc1", "IDLE0", "IDLE1", "Tmr Svc",
    };
    Value lst = mini_nil();
    mini_push_root(vm, &lst);
    for (int i = (int)(sizeof(tasks) / sizeof(tasks[0])) - 1; i >= 0; i--) {
        TaskHandle_t h = xTaskGetHandle(tasks[i]);
        if (!h) continue;
        char key[32];
        snprintf(key, sizeof(key), "stack-free:%s", tasks[i]);
        alist_add(vm, &lst, key, (int64_t)uxTaskGetStackHighWaterMark(h));
    }
    mini_gc(vm);
    alist_add(vm, &lst, "vm-live", (int64_t)mini_get_allocated_bytes(vm));
    alist_add(vm, &lst, "vm-heap", (int64_t)mini_get_heap_size(vm));
    alist_add(vm, &lst, "largest-block", (int64_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    alist_add(vm, &lst, "min-free-heap", (int64_t)esp_get_minimum_free_heap_size());
    alist_add(vm, &lst, "free-heap", (int64_t)esp_get_free_heap_size());
    mini_pop_root(vm);
    return lst;
}

static uint32_t now_ms(void);
static void app_failed(const char *phase, const char *reason);

static void service_display(void) {
    int64_t t0 = esp_timer_get_time();
    s_lcd_us = 0;
    int rows = 0;
    int errors = mini_gui_service(s_vm, &rows);
    if (rows == 0) return;
    s_loop.render_us = esp_timer_get_time() - t0;
    s_loop.lcd_us = s_lcd_us;
    s_loop.render_rows = rows;
    s_frames_acc++;
    s_rows_acc += rows;
    static int s_err_logs = 0;
    if (errors && s_err_logs < 20) {
        s_err_logs++;
        ESP_LOGE(TAG, "Render error: %s", mini_gui_band_last_error());
    }
    if (!errors) mini_device_app_render_ok();
    else if (!s_settings_mode && mini_device_app_error("render", mini_gui_band_last_error(), now_ms())) {
        app_failed("crash", mini_device_app_crash_reason());
    }
    static int s_perf_logs = 0;
    if (s_perf_logs < 8) {
        s_perf_logs++;
        int64_t total = esp_timer_get_time() - t0;
        ESP_LOGI(TAG, "Render %d rows (%s): %lld ms (scheme %lld ms, lcd %lld ms)", rows,
                 gui_mode_name(mini_gui_get_mode()), total / 1000, (total - s_lcd_us) / 1000, s_lcd_us / 1000);
    }
}

/* Trimmed mean of 16 ADC samples: drops min/max so the backlight never flickers */
static int read_light_filtered(void) {
    int sum = 0, lo = 4095, hi = 0;
    for (int i = 0; i < 16; i++) {
        int v = esp32_light_read();
        sum += v;
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    return (sum - lo - hi) / 14;
}

static void read_wall_time(MiniWallTime *t, time_t now) {
    memset(t, 0, sizeof(*t));
    t->synced = hal_time_synced();
    if (!t->synced) return;
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    t->year = tm_now.tm_year + 1900;
    t->month = tm_now.tm_mon + 1;
    t->day = tm_now.tm_mday;
    t->hour = tm_now.tm_hour;
    t->minute = tm_now.tm_min;
    t->second = tm_now.tm_sec;
    t->wday = tm_now.tm_wday;
}

/* ========================================================================= */
/* Application Install (ROM image -> SPIFFS)                                 */
/* ========================================================================= */

static bool file_has_magic(const char *path, const char *magic) {
    char head[4];
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;
    bool ok = fread(head, 1, 4, fp) == 4 && memcmp(head, magic, 4) == 0;
    fclose(fp);
    return ok;
}

/* ========================================================================= */
/* Boot app: [system] boot_app, its metadata and its own config file         */
/* ========================================================================= */


/* boot_app as a /spiffs path: "apps/x.msbc", "/apps/x.msbc" or "/spiffs/apps/x.msbc" */
static bool boot_app_path(const char *value, char *out, size_t cap) {
    if (!value[0]) return false;
    const char *rel = strncmp(value, "/spiffs/", 8) == 0 ? value + 8 : (value[0] == '/' ? value + 1 : value);
    if (snprintf(out, cap, "/spiffs/%s", rel) >= (int)cap) return false;
    return safe_spiffs_path(out);
}

/* Returns true if an application is installed and ready to boot */
static bool has_installed_app(void) {
    char path[64];
    if (boot_app_path(s_sys_cfg.boot_app, path, sizeof(path))) {
        if (strncmp(path, APPS_DIR "/settings/", sizeof(APPS_DIR "/settings/") - 1) != 0 &&
            (file_has_magic(path, "MSBC") || file_has_magic(path, "MFAS"))) {
            return true;
        }
    }
    if (file_has_magic(APP_FASL_PATH, "MFAS") || file_has_magic(APP_MSBC_PATH, "MSBC")) {
        return true;
    }
    return false;
}

/* Picks the app of this boot and makes its config file from its metadata */
static void select_boot_app(void) {
    snprintf(s_app_path, sizeof(s_app_path), "%s", file_has_magic(APP_FASL_PATH, "MFAS") ? APP_FASL_PATH : APP_MSBC_PATH);
    char path[64];
    if (boot_app_path(s_sys_cfg.boot_app, path, sizeof(path))) {
        if (strncmp(path, APPS_DIR "/settings/", sizeof(APPS_DIR "/settings/") - 1) == 0) {
            ESP_LOGW(TAG, "boot_app %s is the system settings; starting %s", path, s_app_path);
        } else if (file_has_magic(path, "MSBC") || file_has_magic(path, "MFAS")) {
            snprintf(s_app_path, sizeof(s_app_path), "%s", path);
        } else {
            ESP_LOGW(TAG, "boot_app %s is not an app image; starting %s", path, s_app_path);
        }
    }
    char *meta = mini_app_meta_read_file(s_app_path);
    mini_app_id(s_app_path, meta, s_app_id, sizeof(s_app_id));
    {   /* [app] storage_kb: an app that keeps files (photos) may ask for more than app_storage_kb */
        char kb[12] = "";
        long v = 0;
        s_app_storage_kb = (meta && mini_device_config_value(meta, "storage_kb", kb, sizeof(kb)) &&
                            parse_long(kb, 64, 1024, &v)) ? (int)v : 0;
    }
    /* [app] api = 1.0 / extensions = esp32: needs this firmware's API */
    if (!mini_api_check_meta(meta, s_app_api_err, sizeof(s_app_api_err))) {
        ESP_LOGE(TAG, "%s: %s", s_app_path, s_app_api_err);
    }
    mini_app_dir_of(s_app_path, s_app_id, s_app_dir, sizeof(s_app_dir));
    snprintf(s_app_ini, sizeof(s_app_ini), "%s/config.ini", s_app_dir);
    const char *defaults = meta ? mini_app_meta_config(meta) : NULL;
    FILE *fp = fopen(s_app_ini, "r");
    if (fp) {
        /* settings an app update added: appended with their defaults */
        char *text = NULL;
        if (defaults && fseek(fp, 0, SEEK_END) == 0) {
            long size = ftell(fp);
            text = size >= 0 && size < 32 * 1024 ? (char *)malloc((size_t)size + 1) : NULL;
            if (text) {
                fseek(fp, 0, SEEK_SET);
                text[fread(text, 1, (size_t)size, fp)] = '\0';
            }
        }
        fclose(fp);
        char *missing = text ? mini_app_ini_missing(text, defaults) : NULL;
        if (missing && missing[0] && (fp = fopen(s_app_ini, "a")) != NULL) {
            size_t n = strlen(text);
            if (n && text[n - 1] != '\n') fputc('\n', fp);
            fputs(missing, fp);
            fclose(fp);
            ESP_LOGI(TAG, "%s: new settings added from the app's defaults", s_app_ini);
        }
        free(missing);
        free(text);
    } else if (defaults) {
        fp = fopen(s_app_ini, "w");
        if (fp) {
            fputs("; settings of this app; they override config.ini\n", fp);
            fputs(defaults, fp);
            fclose(fp);
            ESP_LOGI(TAG, "%s: written from the app's defaults", s_app_ini);
        }
    }
    char name[64];
    if (!meta || !mini_device_config_value(meta, "name", name, sizeof(name))) snprintf(name, sizeof(name), "%s", s_app_id);
    snprintf(s_app_name, sizeof(s_app_name), "%s", name);
    ESP_LOGI(TAG, "Boot app: %s (%s), settings %s", s_app_path, name, s_app_ini);
    free(meta);
}

/* Bytes of the files under dir (an app's storage quota) */
static size_t dir_used_bytes(const char *dir) {
    size_t total = 0;
    DIR *d = opendir(dir);
    for (struct dirent *de = d ? readdir(d) : NULL; de; de = readdir(d)) {
        char path[128];
        struct stat st;
        if (snprintf(path, sizeof(path), "%s/%s", dir, de->d_name) >= (int)sizeof(path) || stat(path, &st) != 0) continue;
        total += S_ISDIR(st.st_mode) ? dir_used_bytes(path) : (size_t)st.st_size;
    }
    if (d) closedir(d);
    return total;
}

/* Firmware before app folders kept the apps flat in /spiffs/apps (<id>.msbc,
 * <id>.ini, <id>.* data): each moves into its folder /spiffs/apps/<id>/, and
 * boot_app with it. The old default device name goes too. */
static void migrate_app_layout(void) {
    DIR *dir = opendir(APPS_DIR);
    char (*names)[48] = dir ? (char (*)[48])malloc(48 * 48) : NULL;
    int n = 0;
    for (struct dirent *de = names ? readdir(dir) : NULL; de && n < 48; de = readdir(dir)) {
        char path[96];
        struct stat st;
        if (snprintf(path, sizeof(path), APPS_DIR "/%s", de->d_name) >= (int)sizeof(path)) continue;
        if (stat(path, &st) == 0 && !S_ISDIR(st.st_mode) && strlen(de->d_name) < 48) snprintf(names[n++], 48, "%s", de->d_name);
    }
    if (dir) closedir(dir);
    for (int i = 0; i < n; i++) {
        char rel[96], from[96], to[112];
        if (!mini_app_legacy_name(names[i], rel, sizeof(rel))) continue;
        snprintf(from, sizeof(from), APPS_DIR "/%s", names[i]);
        snprintf(to, sizeof(to), APPS_DIR "/%s", rel);
        FILE *fp = fopen(to, "rb");
        if (fp) {                       /* already moved once: the folder's copy stays */
            fclose(fp);
            remove(from);
            continue;
        }
        if (rename(from, to) == 0) ESP_LOGI(TAG, "App layout: %s -> %s", from, to);
        else ESP_LOGW(TAG, "App layout: cannot move %s", from);
    }
    free(names);
    char boot[64];
    if (mini_app_legacy_boot(s_sys_cfg.boot_app, boot, sizeof(boot))) {
        const char *key = "boot_app", *val = boot;
        if (config_save("system", &key, &val, 1)) {
            ESP_LOGI(TAG, "boot_app %s -> %s", s_sys_cfg.boot_app, boot);
            snprintf(s_sys_cfg.boot_app, sizeof(s_sys_cfg.boot_app), "%s", boot);
        }
    }
    if (strcmp(s_sys_cfg.device_name, "MiniScheme") == 0) {   /* the old default: the new one */
        const char *key = "device_name", *val = DEFAULT_DEVICE_NAME;
        if (config_save("system", &key, &val, 1)) snprintf(s_sys_cfg.device_name, sizeof(s_sys_cfg.device_name), "%s", val);
    }
}

/* Firmware before the .msbc/.fasl split stored bytecode as app.fasl */
static void migrate_legacy_app(void) {
    if (!file_has_magic(APP_FASL_PATH, "MSBC")) return;
    FILE *fp = fopen(APP_MSBC_PATH, "rb");
    if (fp) {
        fclose(fp);
        remove(APP_FASL_PATH);
    } else if (rename(APP_FASL_PATH, APP_MSBC_PATH) != 0) {
        ESP_LOGW(TAG, "Cannot rename legacy %s", APP_FASL_PATH);
        return;
    }
    ESP_LOGI(TAG, "Legacy bytecode app moved to %s", APP_MSBC_PATH);
}

/* If no app is installed, the device boots directly into the built-in system settings.
 * The firmware no longer forcefully writes a default app into /spiffs/app.msbc. */
static void sync_embedded_app(void) {
    /* No-op: boot into settings if no app is installed */
}

/* ========================================================================= */
/* Factory reset: KEY 18 held from power-on, or the settings' reset menu    */
/* ========================================================================= */

/* Erases the whole app Code ROM partition: the next app boot rebuilds it */
static bool erase_app_rom(void) {
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, APP_ROM_SUBTYPE, NULL);
    if (!p) return false;
    return esp_partition_erase_range(p, 0, p->size) == ESP_OK;
}

/* Removes every file and directory under dir (dir itself stays) */
static int remove_tree(const char *dir) {
    int removed = 0;
    for (int pass = 0; pass < 8; pass++) {   /* entries removed while reading: read again */
        DIR *d = opendir(dir);
        if (!d) break;
        char path[128];
        bool any = false;
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
            if (snprintf(path, sizeof(path), "%s/%s", dir, de->d_name) >= (int)sizeof(path)) continue;
            struct stat st;
            if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
                removed += remove_tree(path);
                if (rmdir(path) == 0) any = true;
            } else if (remove(path) == 0) {
                removed++;
                any = true;
            }
        }
        closedir(d);
        if (!any) break;
    }
    return removed;
}

/* The app configs: <folder>/config.ini of every app (written again from the
 * apps' defaults when they start) */
static int remove_app_configs(void) {
    static const char *const roots[2] = { APPS_DIR, USER_DIR };
    int removed = 0;
    for (int r = 0; r < 2; r++) {
        DIR *d = opendir(roots[r]);
        char (*names)[32] = d ? (char (*)[32])malloc(48 * 32) : NULL;
        int n = 0;
        for (struct dirent *de = names ? readdir(d) : NULL; de && n < 48; de = readdir(d)) {
            if (mini_app_id_ok(de->d_name)) snprintf(names[n++], 32, "%.31s", de->d_name);
        }
        if (d) closedir(d);
        for (int i = 0; i < n; i++) {
            char path[96];
            snprintf(path, sizeof(path), "%s/%s/config.ini", roots[r], names[i]);
            if (remove(path) == 0) removed++;
        }
        free(names);
    }
    return removed;
}

/* Every font of the system font ROM (fonts in use are dropped first) */
static bool clear_font_rom(void) {
    if (!mini_fontrom_present()) return true;
    for (int id = 1; id < MINI_FONT_SLOTS; id++) mini_font_unload(id);
    for (int guard = 0; mini_fontrom_count() > 0 && guard < 64; guard++) {
        const char *entry = mini_fontrom_entry(0, NULL, NULL);
        char name[48];
        if (!entry) return false;
        snprintf(name, sizeof(name), "%s", entry);   /* the directory changes on removal */
        if (mini_fontrom_remove(name)) return false;
    }
    return true;
}

/* (factory-reset what) in the system settings: what is rom, configs, fonts,
 * files or all (the app ROM and every file; the font ROM stays: it is part
 * of the factory image and the settings need it for Chinese text). #t, then
 * the board restarts into the built-in settings 1.5 s later; else a string */
static Value prim_factory_reset(MiniVM *vm, int argc, Value *argv) {
    if (!mini_access_privileged(vm, "factory-reset")) return mini_void();
    if (argc != 1 || !mini_is_symbol(argv[0])) {
        vm_error(vm, "factory-reset: expected 'rom, 'configs, 'fonts, 'files or 'all");
        return mini_void();
    }
    if (!s_settings_mode) return mini_make_string(vm, "factory-reset: only in the system settings");
    const char *what = mini_symbol_name(argv[0]);
    bool all = strcmp(what, "all") == 0, ok = true;
    if (all || strcmp(what, "rom") == 0) ok = erase_app_rom() && ok;
    if (all || strcmp(what, "files") == 0) {
        int n = remove_tree("/spiffs");
        ESP_LOGW(TAG, "Factory reset: %d files removed", n);
    } else if (strcmp(what, "configs") == 0) {
        ESP_LOGW(TAG, "Factory reset: %d app configs removed", remove_app_configs());
    } else if (strcmp(what, "fonts") == 0) {
        ok = clear_font_rom();
    } else if (!all && strcmp(what, "rom") != 0) {
        return mini_make_string(vm, "factory-reset: expected rom, configs, fonts, files or all");
    }
    ESP_LOGW(TAG, "Factory reset (%s): %s", what, ok ? "done" : "failed");
    if (!ok) return mini_make_string(vm, "factory-reset: flash erase failed");
    s_builtin_boot = BUILTIN_SETTINGS_MAGIC;
    s_settings_boot = SETTINGS_BOOT_MAGIC;
    s_factory_restart_at = esp_timer_get_time() + 1500000;
    return mini_true();
}

/* KEY 18 already held at power-on ([system] factory_reset_key): a 10 s
 * countdown on the screen; released early, the boot goes on. Held to the
 * end: the app ROM is erased and this boot opens the built-in settings. */
#define FACTORY_HOLD_MS 10000
static void factory_hold_at_boot(void) {
    if (!s_sys_cfg.factory_reset_key || !esp32_button_read()) return;
    int64_t t0 = esp_timer_get_time();
    int shown = -1;
    char reason[160];
    ESP_LOGW(TAG, "KEY 18 held at power-on: factory reset in %d s", FACTORY_HOLD_MS / 1000);
    while (esp32_button_read()) {
        int64_t held_ms = (esp_timer_get_time() - t0) / 1000;
        if (held_ms >= FACTORY_HOLD_MS) break;
        int left = (int)((FACTORY_HOLD_MS - held_ms + 999) / 1000);
        if (left != shown && held_ms >= 300) {   /* not for a bounce */
            shown = left;
            snprintf(reason, sizeof(reason), "\xe7\xbb\xa7\xe7\xbb\xad\xe6\x8c\x89\xe4\xbd\x8f %d \xe7\xa7\x92\xef\xbc\x9a\xe6\xb8\x85\xe7\xa9\xba\xe7\xa8\x8b\xe5\xba\x8f ROM\xef\xbc\x8c\xe8\xbf\x9b\xe5\x85\xa5\xe5\x86\x85\xe7\xbd\xae\xe7\x9a\x84\xe7\xb3\xbb\xe7\xbb\x9f\xe8\xae\xbe\xe7\xbd\xae\xe3\x80\x82\xe6\x9d\xbe\xe5\xbc\x80\xe6\x8c\x89\xe9\x94\xae\xe5\x8f\x96\xe6\xb6\x88\xe3\x80\x82", left);
            mini_device_error_screen("\xe6\x81\xa2\xe5\xa4\x8d\xe5\x87\xba\xe5\x8e\x82\xe8\xae\xbe\xe7\xbd\xae", "\xe6\x8c\x89\xe4\xbd\x8f\xe6\x8c\x89\xe9\x94\xae", reason);
            mini_ws2812_set((left & 1) ? 255 : 80, 60, 0);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if ((esp_timer_get_time() - t0) / 1000 < FACTORY_HOLD_MS) {
        if (shown >= 0) ESP_LOGI(TAG, "Factory reset cancelled");
        mini_ws2812_set(0, 255, 255);
        return;
    }
    mini_device_error_screen("\xe6\x81\xa2\xe5\xa4\x8d\xe5\x87\xba\xe5\x8e\x82\xe8\xae\xbe\xe7\xbd\xae", "\xe6\xad\xa3\xe5\x9c\xa8\xe6\xb8\x85\xe7\xa9\xba\xe7\xa8\x8b\xe5\xba\x8f ROM", "");
    bool ok = erase_app_rom();
    ESP_LOGW(TAG, "Factory reset at power-on: app ROM %s", ok ? "erased" : "not erased");
    s_settings_boot = SETTINGS_BOOT_MAGIC;   /* read below: this boot runs the settings */
    s_builtin_settings = true;
#if MINISCHEME_FW_HTTP
    if (!s_http_started) start_http_server();   /* web_in_app = 0: the settings still get their page */
#endif
    mini_ws2812_set(255, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(800));
}

/* ========================================================================= */
/* Code ROM: the app's bytecode and literals run in place from Flash        */
/* ========================================================================= */

#define APP_ROM_FORMAT  1u   /* bump to force a rebuild after layout changes */

static const esp_partition_t *s_rom_part = NULL;
static const void *s_rom_ptr = NULL;
static esp_partition_mmap_handle_t s_rom_map;

/* Image key: app bytes + firmware build, so a change to either rebuilds */
static uint32_t app_rom_key(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    uint32_t h = 2166136261u ^ APP_ROM_FORMAT;
    uint8_t buf[256];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        for (size_t i = 0; i < n; i++) { h ^= buf[i]; h *= 16777619u; }
    }
    fclose(fp);
    const esp_app_desc_t *desc = esp_app_get_description();
    for (int i = 0; i < 8; i++) { h ^= desc->app_elf_sha256[i]; h *= 16777619u; }
    return h ? h : 1;
}

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

typedef struct {
    const esp_partition_t *part;
    size_t erased; /* bytes erased so far, sector aligned */
} RomFlashSink;

static bool rom_flash_write(size_t off, const void *data, size_t len, void *user) {
    RomFlashSink *rs = (RomFlashSink *)user;
    if (off + len > rs->part->size) return false;
    size_t need = (off + len + 4095) & ~(size_t)4095;
    if (need > rs->erased) {
        if (esp_partition_erase_range(rs->part, rs->erased, need - rs->erased) != ESP_OK) return false;
        rs->erased = need;
    }
    return esp_partition_write(rs->part, off, data, len) == ESP_OK;
}

/* Maps the image partition; true when the image there matches key */
static bool map_app_rom(uint32_t key) {
    s_rom_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, APP_ROM_SUBTYPE, NULL);
    mini_lib_set_app_rom_capacity(s_rom_part ? s_rom_part->size : 0);
    if (!s_rom_part) {
        ESP_LOGW(TAG, "No approm partition: app runs from RAM");
        return false;
    }
    if (esp_partition_mmap(s_rom_part, 0, s_rom_part->size, ESP_PARTITION_MMAP_DATA,
                           &s_rom_ptr, &s_rom_map) != ESP_OK) {
        ESP_LOGW(TAG, "approm mmap failed: app runs from RAM");
        s_rom_ptr = NULL;
        return false;
    }
    return mini_rom_check(s_rom_ptr, s_rom_part->size, key);
}

/* ========================================================================= */
/* System font ROM: MFNT fonts read in place from a mapped partition        */
/* ========================================================================= */

#define FONT_ROM_SUBTYPE 0x41 /* "fontrom" partition, see partitions.csv */

static const esp_partition_t *s_font_part = NULL;

static bool font_rom_erase(size_t off, size_t len, void *user) {
    (void)user;
    return esp_partition_erase_range(s_font_part, off, len) == ESP_OK;
}

static bool font_rom_write(size_t off, const void *data, size_t len, void *user) {
    (void)user;
    return esp_partition_write(s_font_part, off, data, len) == ESP_OK;
}

/* Writes invalidate the cache of the mapped range, so the mapping always
 * shows the current flash contents */
static void map_font_rom(void) {
    static esp_partition_mmap_handle_t map;
    const void *ptr = NULL;
    s_font_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, FONT_ROM_SUBTYPE, NULL);
    if (!s_font_part) {
        ESP_LOGW(TAG, "No fontrom partition: fonts come from apps and SPIFFS only");
        return;
    }
    if (esp_partition_mmap(s_font_part, 0, s_font_part->size, ESP_PARTITION_MMAP_DATA, &ptr, &map) != ESP_OK) {
        ESP_LOGW(TAG, "fontrom mmap failed");
        return;
    }
    MiniFontRomFlash flash = { (const uint8_t *)ptr, s_font_part->size, font_rom_erase, font_rom_write, NULL };
    mini_fontrom_attach(&flash);
    ESP_LOGI(TAG, "Font ROM: %d fonts, %u / %u KB used", mini_fontrom_count(),
             (unsigned)(mini_fontrom_used() / 1024), (unsigned)(mini_fontrom_capacity() / 1024));
}

/* config.ini [fonts]: source order and replacements of fonts. A file from
 * older firmware gets the section appended (its other settings are kept). */
static void apply_font_config(const char *path) {
    char *text = NULL;
    FILE *fp = fopen(path, "r");
    if (fp) {
        fseek(fp, 0, SEEK_END);
        long n = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        text = (n > 0 && n < 16384) ? (char *)calloc(1, (size_t)n + 1) : NULL;
        if (text && fread(text, 1, (size_t)n, fp) == 0) text[0] = '\0';
        fclose(fp);
    }
    char probe[8];
    if (text && !mini_device_config_value(text, "font_order", probe, sizeof(probe))) {
        fp = fopen(path, "a");
        if (fp) {
            fputs(mini_device_config_fonts_section, fp);
            fclose(fp);
            ESP_LOGI(TAG, "%s: added the [fonts] section", path);
        }
    }
    const char *err = mini_device_apply_font_config(text);
    mini_device_apply_text_config(text);
    char order[160], over[400];
    mini_font_config_text(order, sizeof(order), over, sizeof(over));
    if (err) ESP_LOGW(TAG, "config.ini [fonts]: %s (using defaults)", err);
    ESP_LOGI(TAG, "Fonts: order = %s; override = %s", order, over[0] ? over : "none");
    free(text);
}

/* The MSBC file for the streaming build; its progress drives the bar
 * (reading and writing go together, so bytes read measure the work) */
typedef struct {
    FILE *fp;
    size_t total, done;
} RomFileIn;

static size_t rom_file_read(void *buf, size_t n, void *user) {
    RomFileIn *in = (RomFileIn *)user;
    size_t k = fread(buf, 1, n, in->fp);
    in->done += k;
    if (in->total) mini_device_progress((int)((uint64_t)990 * (in->done < in->total ? in->done : in->total) / in->total), now_ms());
    return k;
}

/* Builds the image straight from the file (mini_rom_build_stream): a few KB
 * of memory whatever the app's size, nothing left fragmented, so the boot
 * goes on without a restart. Its failure is final: the bytecode itself is
 * bad (any other builder would fail on it too). */
static bool stream_app_rom(uint32_t key, const char *path) {
    int64_t t0 = esp_timer_get_time();
    RomFileIn in = { fopen(path, "rb"), 0, 0 };
    if (!in.fp) return false;
    fseek(in.fp, 0, SEEK_END);
    in.total = (size_t)ftell(in.fp);
    fseek(in.fp, 0, SEEK_SET);
    mini_device_progress(0, now_ms());
    RomFlashSink rs = { s_rom_part, 0 };
    size_t peak = 0;
    const char *err = NULL;
    size_t size = mini_rom_build_stream(rom_file_read, &in, (uintptr_t)s_rom_ptr, s_rom_ptr, key,
                                        rom_flash_write, &rs, &peak, &err);
    fclose(in.fp);
    bool ok = size && mini_rom_check(s_rom_ptr, s_rom_part->size, key);
    if (ok) {
        ESP_LOGI(TAG, "App ROM streamed: %u B in %d ms, %u B of memory", (unsigned)size,
                 (int)((esp_timer_get_time() - t0) / 1000), (unsigned)peak);
        mini_device_progress(1000, now_ms());
        vTaskDelay(pdMS_TO_TICKS(200));  /* the full bar, then the app */
    } else {
        snprintf(s_rom_err, sizeof(s_rom_err), "%s", err ? err : (size ? "rom: image fails verification" : "rom: build failed"));
        ESP_LOGW(TAG, "App ROM stream build failed: %s", s_rom_err);
    }
    mini_device_progress_end();
    return ok;
}

/* The app VM with every firmware library registered */
/* ---- The esp32 extension API (api/spec/ext_esp32.py) ---- */

/* (esp32-mac) => "aa:bb:cc:dd:ee:ff", the factory WiFi station MAC */
static Value prim_esp32_mac(MiniVM *vm, int argc, Value *argv) {
    (void)argc; (void)argv;
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char buf[20];
    snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return mini_make_string(vm, buf);
}

/* (esp32-reset-reason) => why the chip last started, a symbol */
static Value prim_esp32_reset_reason(MiniVM *vm, int argc, Value *argv) {
    (void)argc; (void)argv;
    const char *r = "other";
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON: r = "power-on"; break;
    case ESP_RST_SW: r = "software"; break;
    case ESP_RST_PANIC: r = "panic"; break;
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: r = "watchdog"; break;
    case ESP_RST_BROWNOUT: r = "brownout"; break;
    case ESP_RST_DEEPSLEEP: r = "deep-sleep"; break;
    default: break;
    }
    return mini_make_symbol(vm, r);
}

/* (esp32-idf-version) => "v6.1" */
static Value prim_esp32_idf_version(MiniVM *vm, int argc, Value *argv) {
    (void)argc; (void)argv;
    return mini_make_string(vm, esp_get_idf_version());
}

static const MiniPrimEntry ESP32_EXT_PRIMS[] = {
    { "esp32-mac", prim_esp32_mac },
    { "esp32-reset-reason", prim_esp32_reset_reason },
    { "esp32-idf-version", prim_esp32_idf_version },
};

static MiniVM *create_app_vm(size_t heap) {
    MiniConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.initial_heap_bytes = heap;
    cfg.max_heap_bytes     = heap;
    cfg.eden_bytes         = s_sys_cfg.vm_eden;
    cfg.stack_size         = s_sys_cfg.vm_stack;
    cfg.frame_capacity     = s_sys_cfg.vm_frames;
    cfg.skip_init_fasl     = true;
    MiniVM *vm = mini_create_ext(&cfg);
    if (!vm) return NULL;
    mini_register_hardware_lib(vm);
    mini_register_gui_lib(vm);
    mini_register_help_lib(vm);
    mini_register_device_lib(vm);
    mini_register_json_feed_lib(vm);
    mini_register_c_func(vm, "config-get", prim_config_get);
    mini_register_c_func(vm, "config-set!", prim_config_set);
    mini_register_c_func(vm, "app-config-set!", prim_app_config_set);
    mini_register_c_func(vm, "app-info", prim_app_info);
    mini_register_c_func(vm, "app-list", prim_app_list);
    mini_register_c_func(vm, "factory-reset", prim_factory_reset);
    mini_register_c_func(vm, "setup-mode-start", prim_setup_mode_start);
    mini_register_c_func(vm, "setup-mode-status", prim_setup_mode_status);
    mini_register_c_func(vm, "fs-info", prim_fs_info);
    mini_register_c_func(vm, "fs-list", prim_fs_list);
    mini_register_c_func(vm, "last-app-stats", prim_last_app_stats);
    mini_register_c_func(vm, "display-bench", prim_display_bench);
    mini_register_c_func(vm, "tick-bench", prim_tick_bench);
    mini_register_c_func(vm, "mem-report", prim_mem_report);
    mini_register_prim_table(vm, ESP32_EXT_PRIMS, sizeof(ESP32_EXT_PRIMS) / sizeof(ESP32_EXT_PRIMS[0]));
    mini_register_market_lib(vm);
    return vm;
}

/* config.ini with keys of one section set (comments kept); written to a
 * temporary file first, then renamed over the old one */
static bool config_save(const char *section, const char *const *keys, const char *const *values, int n) {
    const char *path = "/spiffs/config.ini", *tmp = "/spiffs/config.tmp";
    FILE *fp = fopen(path, "rb");
    long size = 0;
    if (fp) {
        fseek(fp, 0, SEEK_END);
        size = ftell(fp);
        fseek(fp, 0, SEEK_SET);
    }
    if (size < 0 || size > 32 * 1024) size = 0;
    char *text = (char *)malloc((size_t)size + 1);
    if (!text) {
        if (fp) fclose(fp);
        return false;
    }
    size_t got = fp ? fread(text, 1, (size_t)size, fp) : 0;
    if (fp) fclose(fp);
    text[got] = '\0';
    for (int i = 0; i < n && text; i++) {
        char *next = mini_device_config_set(text, section, keys[i], values[i]);
        free(text);
        text = next;
    }
    if (!text) return false;
    fp = fopen(tmp, "wb");
    size_t len = strlen(text);
    bool ok = fp && fwrite(text, 1, len, fp) == len;
    if (fp) ok = fclose(fp) == 0 && ok;
    free(text);
    if (ok) ok = rename(tmp, path) == 0;
    if (!ok) remove(tmp);
    return ok;
}

/* Main task: the new color bits / scheme are drawn at once and kept for the
 * next boot */
static void job_display(RemoteJob *job) {
    if (job->bits) mini_gui_set_color_bits(job->bits);
    if (job->scheme >= 0) mini_gui_set_scheme((MiniGuiScheme)job->scheme);
    s_sys_cfg.color_bits = mini_gui_get_color_bits();
    s_sys_cfg.scheme = mini_gui_get_scheme();
    mini_gui_invalidate(0, mini_gui_get_height());
    char bits[8];
    snprintf(bits, sizeof(bits), "%d", s_sys_cfg.color_bits);
    const char *keys[2] = { "color_bits", "palette" };
    const char *values[2] = { bits, mini_gui_scheme_name(s_sys_cfg.scheme) };
    bool saved = config_save("display", keys, values, 2);
    ESP_LOGI(TAG, "Display: %d-bit %s from the WebREPL (%s config.ini)", s_sys_cfg.color_bits, values[1],
             saved ? "saved to" : "NOT saved to");
    snprintf(job->out, job->out_cap,
             "{\"success\":true,\"color_bits\":%d,\"palette\":\"%s\",\"buffer_bytes\":%u,\"saved\":%s}",
             s_sys_cfg.color_bits, values[1], (unsigned)mini_gui_get_fb_ram(), saved ? "true" : "false");
}

/* ========================================================================= */
/* File System: LittleFS on the "storage" partition, mounted at /spiffs      */
/* ========================================================================= */

#define STORAGE_LABEL       "storage"
#define STORAGE_MAX_FILES   6   /* ~360 B each, allocated at mount */
static bool storage_info(size_t *total, size_t *used) {
    return lfsvfs_info(total, used) == ESP_OK;
}

/* LittleFS; a partition without one (a new board) is formatted. Firmware
 * from before 2026-10-09 kept SPIFFS here and migrated it on its first
 * LittleFS boot; that copy is gone (22 KB of flash): such a board starts
 * with an empty file system and the default config.ini. */
static void mount_storage(void) {
    esp32_set_storage_info(storage_info);
    esp_err_t r = lfsvfs_mount("/spiffs", STORAGE_LABEL, false, STORAGE_MAX_FILES);
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "No LittleFS on '%s': formatting", STORAGE_LABEL);
        r = lfsvfs_format(STORAGE_LABEL);
        if (r == ESP_OK) r = lfsvfs_mount("/spiffs", STORAGE_LABEL, false, STORAGE_MAX_FILES);
    }
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount LittleFS (%s)", esp_err_to_name(r));
        return;
    }
    size_t total = 0, used = 0;
    lfsvfs_info(&total, &used);
    ESP_LOGI(TAG, "LittleFS mounted at /spiffs: total=%u B, used=%u B", (unsigned)total, (unsigned)used);
}

/* (backlight-set) / (backlight-auto) from Scheme: kept in config.ini, written
 * once the changes stop for 3 s (an app setting it often wears no flash) */
static volatile bool s_bl_pending = false;
static volatile int64_t s_bl_changed_us = 0;

static void backlight_changed(bool auto_on, int level) {
    s_sys_cfg.auto_backlight = auto_on;
    if (!auto_on) s_sys_cfg.backlight = level;
    s_bl_pending = true;
    s_bl_changed_us = esp_timer_get_time();
}

static void backlight_save_pending(void) {
    if (!s_bl_pending || esp_timer_get_time() - s_bl_changed_us < 3000000) return;
    s_bl_pending = false;
    char level[8];
    snprintf(level, sizeof(level), "%d", s_sys_cfg.backlight);
    const char *keys[2] = { "auto_backlight", "backlight" };
    const char *values[2] = { s_sys_cfg.auto_backlight ? "1" : "0", level };
    if (!config_save("display", keys, values, 2)) ESP_LOGW(TAG, "Backlight setting not saved");
}

static void record_last_app(void) {
    memset(&s_last_app, 0, sizeof(s_last_app));
    snprintf(s_last_app.id, sizeof(s_last_app.id), "%s", s_app_id);
    if (s_vm) {
        MiniStats st;
        mini_get_stats(s_vm, &st);
        s_last_app.heap = (uint32_t)mini_get_heap_size(s_vm);
        s_last_app.live = (uint32_t)st.live_bytes;
        s_last_app.peak = (uint32_t)st.live_peak;
        s_last_app.alloc_kb = (uint32_t)(st.alloc_bytes / 1024);
        s_last_app.rom = (uint32_t)st.rom_bytes;
    }
    s_last_app.uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    s_last_app.free_heap = (uint32_t)esp_get_free_heap_size();
    s_last_app.min_free = (uint32_t)esp_get_minimum_free_heap_size();
    s_last_app.magic = LAST_APP_MAGIC;
}

/* The app could not be built (phase "rom"), loaded ("load") or keeps
 * failing ("crash"): the reason on the screen for 5 s, then the system
 * settings, which show it again. The settings themselves never come here
 * (that would loop). */
static void app_failed(const char *phase, const char *reason) {
    ESP_LOGE(TAG, "App %s failure (%s): %s", phase, s_app_path, reason ? reason : "?");
    if (s_settings_mode) return;
    char why[120];
    snprintf(why, sizeof(why), "%s", reason ? reason : "");
    record_last_app();
    snprintf(s_last_app.phase, sizeof(s_last_app.phase), "%s", phase);
    snprintf(s_last_app.error, sizeof(s_last_app.error), "%s", why);
    mini_gui_set_hold_overlay(0, 0, 0, 0);
    mini_device_error_screen(mini_device_fail_title(phase), s_app_name[0] ? s_app_name : s_app_id, why);
    vTaskDelay(pdMS_TO_TICKS(5000));
    s_settings_boot = SETTINGS_BOOT_MAGIC;
    esp_restart();
}

/* After an app hook: an error counts towards a crash (see mini_device_app_error) */
static void check_app_error(const char *where) {
    const char *err = mini_get_error(s_vm);
    if (err && !s_settings_mode && mini_device_app_error(where, err, now_ms())) {
        mini_clear_error(s_vm);
        app_failed("crash", mini_device_app_crash_reason());
    }
    mini_clear_error(s_vm);
}

/* A mid press on the hardware pages: restart into the system settings, which get a whole VM
 * heap and leave the app's Flash image alone; leaving them restarts into
 * the boot app */

static void open_system_settings(void) {
    if (s_settings_mode) return;
    ESP_LOGI(TAG, "Restarting into the system settings");
    record_last_app();
    s_settings_boot = SETTINGS_BOOT_MAGIC;
    mini_ws2812_set(160, 0, 255);
    vTaskDelay(pdMS_TO_TICKS(30));
    esp_restart();
}

/* ========================================================================= */
/* Entry Point                                                               */
/* ========================================================================= */

void app_main(void) {
    mini_api_set_extensions(mini_api_extension_bit("esp32"));   /* this board's extension APIs */
    market_host_init();
    /* Claim the largest DRAM block for the VM heap first, before NVS, SPIFFS,
     * display, DMA and WiFi buffers split it; released right before the VM
     * is created, whose size is still bounded by MIN_OS_RESERVE */
    size_t hold_bytes = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (hold_bytes > VM_HEAP_HOLD_MAX) hold_bytes = VM_HEAP_HOLD_MAX;
    void *vm_heap_hold = heap_caps_malloc(hold_bytes, MALLOC_CAP_8BIT);

    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, "MiniScheme Embedded OS - ESP32 3.5\" Desktop Display");
    ESP_LOGI(TAG, "==================================================");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* 1. Mount the file system (LittleFS, at /spiffs for compatibility) */
    mount_storage();

    /* 2. Config + display buffer BEFORE the VM so the buffer stays contiguous */
    config_append_missing("/spiffs/config.ini", "boot_app", mini_device_config_system_section);
    config_append_missing("/spiffs/config.ini", "marquee", mini_device_config_text_section);
    config_append_missing("/spiffs/config.ini", "channel_server", mini_device_config_channel_section);
    char wm_probe[8];
    if (!ini_file_value("/spiffs/config.ini", "wrap_marquee", wm_probe, sizeof(wm_probe))) {
        /* [text] from before wrapped text had its own marquee */
        const char *keys[3] = { "wrap_marquee", "wrap_marquee_fps", "wrap_marquee_step" };
        const char *values[3] = { "line", "0.5", "1" };
        config_save("text", keys, values, 3);
    }
    if (!ini_file_value("/spiffs/config.ini", "marquee_hold", wm_probe, sizeof(wm_probe))) {
        const char *keys[2] = { "marquee_hold", "wrap_marquee_hold" };
        const char *values[2] = { "1", "1" };
        config_save("text", keys, values, 2);
    }
    load_or_create_config("/spiffs/config.ini", &s_sys_cfg);
    s_http_stack = s_sys_cfg.http_stack;
    if (s_sys_cfg.color_fmt == MINI_GUI_COLOR_4BPP) s_sys_cfg.color_fmt = MINI_GUI_COLOR_RGB565;
    bool rgb666 = s_sys_cfg.color_fmt == MINI_GUI_COLOR_RGB666;
    MiniGuiConfig gcfg = {
        .width = s_sys_cfg.width, .height = s_sys_cfg.height, .mode = s_sys_cfg.mode,
        .band_lines = s_sys_cfg.band_lines, .color_bits = s_sys_cfg.color_bits,
        .scheme = s_sys_cfg.scheme, .bus = s_sys_cfg.color_fmt,
    };
    /* A full frame comes out of the block held for the VM: allocated from
     * the fragments left beside it, it would starve WiFi instead */
    bool full_frame = s_sys_cfg.mode == MINI_GUI_MODE_FULL;
    if (full_frame) {
        free(vm_heap_hold);
        vm_heap_hold = NULL;
    }
    bool gui_ok = mini_gui_configure(&gcfg);
    if (full_frame) {
        /* Hold what is left for the VM, minus what WiFi, lwIP and the HTTP
         * server need while they start (measured: ~50 KB) */
        size_t free_now = esp_get_free_heap_size();
        hold_bytes = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        if (hold_bytes > VM_HEAP_HOLD_MAX) hold_bytes = VM_HEAP_HOLD_MAX;
        if (hold_bytes + OS_INIT_RESERVE > free_now) {
            hold_bytes = free_now > OS_INIT_RESERVE ? free_now - OS_INIT_RESERVE : 0;
        }
        vm_heap_hold = hold_bytes ? heap_caps_malloc(hold_bytes, MALLOC_CAP_8BIT) : NULL;
    }
    if (!gui_ok) {
        ESP_LOGW(TAG, "Display: no RAM for the %s buffer at %d bits, using %s mode",
                 gui_mode_name(s_sys_cfg.mode), s_sys_cfg.color_bits, gui_mode_name(mini_gui_get_mode()));
    }
    s_sys_cfg.mode = mini_gui_get_mode();
    bool band = s_sys_cfg.mode == MINI_GUI_MODE_BAND;

    /* Panel init (bit-banged), then hand the bus to I2S DMA while the heap is
     * still unfragmented: its DMA copy buffer must be internal and contiguous */
    esp32_ili9488_init();
    esp32_ili9488_set_pixel_format(0x55);
    /* DIRECT mode sends thousands of small fills: a DMA transaction costs far
     * more than bit-banging a few pixels (measured 590 ms vs 165 ms a frame) */
    if (s_sys_cfg.lcd_dma && s_sys_cfg.mode == MINI_GUI_MODE_DIRECT) {
        ESP_LOGI(TAG, "Direct mode: GPIO bit-bang bus (faster than DMA for small fills)");
    } else if (s_sys_cfg.lcd_dma) {
        /* The driver keeps two copies of a chunk, plus a staging copy when rows
         * are converted: chunks stay small even when a low-bit band is tall */
        int lines = band && mini_gui_get_band_lines() < DEF_BAND_LINES ? mini_gui_get_band_lines() : DEF_BAND_LINES;
        int chunk = (s_sys_cfg.lcd_dma_lines > 0 && s_sys_cfg.lcd_dma_lines < lines) ? s_sys_cfg.lcd_dma_lines : lines;
        size_t chunk_bytes = (size_t)s_sys_cfg.width * (size_t)chunk * (rgb666 ? 3u : 2u);
        if (!esp32_ili9488_start_dma(chunk_bytes, s_sys_cfg.lcd_clock_hz, rgb666)) {
            ESP_LOGW(TAG, "LCD DMA unavailable, using GPIO bit-bang transfers");
        }
    }
    esp32_ili9488_set_bus_rgb666(rgb666);
    /* Palette rows are expanded on the way out */
    if (mini_gui_get_color_bits() < 16 && !(s_sys_cfg.mode == MINI_GUI_MODE_DIRECT)) esp32_ili9488_reserve_staging();
    mini_gui_set_output(lcd_emit_rows, &s_lcd_sink, NULL);
    mini_gui_set_rect_output(lcd_emit_rect);
    ESP_LOGI(TAG, "Display: %s, %d-bit %s, %s bus, buffer %u B",
             gui_mode_name(s_sys_cfg.mode), mini_gui_get_color_bits(), mini_gui_scheme_name(mini_gui_get_scheme()),
             rgb666 ? "rgb666" : "rgb565", (unsigned)mini_gui_get_fb_ram());
    ESP_LOGI(TAG, "Heap after GUI buffer: free=%u B, largest=%u B, buffer=%u B",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned)mini_gui_get_fb_ram());

    /* 3. WiFi, SNTP and the HTTP API (started before the heap probe) */
    esp32_wifi_early_init();
    esp32_sntp_init();
    hal_wifi_connect(s_sys_cfg.wifi_ssid, s_sys_cfg.wifi_pass);
    s_job_lock = xSemaphoreCreateMutex();
    s_job_done = xSemaphoreCreateBinary();
#if MINISCHEME_FW_HTTP
    /* [system] web_in_app = 0: no web page while an app runs (its RAM goes
     * to the app's VM); the system settings always have it */
    if (s_sys_cfg.web_in_app || s_settings_boot == SETTINGS_BOOT_MAGIC) {
        start_http_server();
    } else {
        ESP_LOGI(TAG, "HTTP server off while the app runs ([system] web_in_app = 0)");
    }
#endif

    /* 4. Peripherals */
    esp32_backlight_init();
    mini_backlight_set((uint8_t)s_sys_cfg.backlight);
    mini_backlight_auto_set(s_sys_cfg.auto_backlight);
    mini_backlight_set_hook(backlight_changed);
    esp32_ws2812_init();
    mini_ws2812_set(0, 255, 255);
    esp32_light_init();
    esp32_button_init();

    map_font_rom();
    apply_font_config("/spiffs/config.ini");
    factory_hold_at_boot();
    {   /* apps' https:// feeds go through the protocol converter ([app] https_proxy, else proxy) */
        char *cfg = read_small_file("/spiffs/config.ini", 32 * 1024);
        char conv[128] = "";
        if (cfg && (!mini_device_config_value(cfg, "https_proxy", conv, sizeof(conv)) || !conv[0])) {
            mini_device_config_value(cfg, "proxy", conv, sizeof(conv));
        }
        mini_json_feed_set_https_proxy(conv);
        setup_channel(cfg);
        free(cfg);
    }

    /* 5. Install the app and make sure its Flash (XIP) image is current.
     * The check runs while the VM block is still held so its small
     * allocations land elsewhere; only a rebuild needs that block. */
    migrate_legacy_app();
    migrate_app_layout();
    sync_embedded_app();
    s_settings_mode = s_settings_boot == SETTINGS_BOOT_MAGIC;
    s_settings_boot = 0; /* the next restart runs the boot app again */
    if (s_builtin_boot == BUILTIN_SETTINGS_MAGIC) s_builtin_settings = s_settings_mode;
    s_builtin_boot = 0;
    if (!s_settings_mode && !has_installed_app()) {
        ESP_LOGI(TAG, "No app installed; starting built-in system settings");
        s_settings_mode = true;
        s_builtin_settings = true;
    }
    if (!s_settings_mode) s_last_app.magic = 0; /* the app's own figures are live again */
    mini_device_button_system_gesture(!s_settings_mode); /* in the settings a long hold is a long press */
    if (s_settings_mode) {
        /* the settings run from RAM: the boot app's Flash image stays valid */
        snprintf(s_app_path, sizeof(s_app_path), "(system settings)");
        snprintf(s_app_id, sizeof(s_app_id), "settings");
        snprintf(s_app_dir, sizeof(s_app_dir), APPS_DIR "/settings");
        snprintf(s_app_ini, sizeof(s_app_ini), APPS_DIR "/settings/config.ini");
        ESP_LOGI(TAG, "Boot app: the system settings");
    } else {
        select_boot_app();
        if (s_app_api_err[0]) {
            free(vm_heap_hold);
            vm_heap_hold = NULL;
            app_failed("api", s_app_api_err);
        }
    }
    const char *app_path = s_app_path;
    bool fasl_app = !s_settings_mode && file_has_magic(app_path, "MFAS");
    if (!s_settings_mode && mini_app_web_locate_file(app_path, &s_app_web_at, &s_app_web_len)) {
        ESP_LOGI(TAG, "App web page: %u B at /app", (unsigned)s_app_web_len);
    }
    uint32_t rom_key = (fasl_app || s_settings_mode) ? 0 : app_rom_key(app_path);
    if (!s_sys_cfg.app_xip) rom_key = 0; /* app_xip = 0: always load to RAM */
    bool rom_ok = rom_key && map_app_rom(rom_key);
    /* The streaming build needs a few KB: it runs while the VM's block is
     * still held, so nothing it allocates can split that block */
    if (rom_key && !rom_ok && s_rom_ptr) {
        rom_ok = stream_app_rom(rom_key, app_path);
        if (!rom_ok) {
            free(vm_heap_hold);
            vm_heap_hold = NULL;
            app_failed("rom", s_rom_err[0] ? s_rom_err : "rom: build failed");
        }
    }

    /* 6. Size the VM heap from what is actually free, once WiFi has joined:
     * association and DHCP take a few KB, and measured before or after them
     * the heap came out a step apart from boot to boot. The VM's block stays
     * held until then: what WiFi, DHCP, SNTP and the location lookup
     * allocate meanwhile lands elsewhere, not in the middle of it (a split
     * block once left the VM 93 KB instead of 102). An absent network
     * delays the boot by WIFI_PROBE_WAIT_MS at most. */
    if (s_sys_cfg.wifi_ssid[0]) {
        int64_t until = esp_timer_get_time() + (int64_t)WIFI_PROBE_WAIT_MS * 1000;
        while (!esp32_wifi_connected() && esp_timer_get_time() < until) vTaskDelay(pdMS_TO_TICKS(50));
        ESP_LOGI(TAG, "Heap probe %s", esp32_wifi_connected() ? "after WiFi joined" : "without WiFi (not joined in time)");
    }
    free(vm_heap_hold);
    size_t free_heap = esp_get_free_heap_size();
    size_t largest_8bit = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    size_t reserved = s_sys_cfg.reserved_heap; /* [memory] reserved_heap */
    size_t avail = (free_heap > reserved) ? (free_heap - reserved) : 16384;
    /* the VM struct and eden may be carved from the same block */
    if (avail > (largest_8bit - 8192)) {
        avail = (largest_8bit > 8192) ? (largest_8bit - 8192) : 32768;
    }
    avail = (avail / 1024) * 1024;   /* KB steps: 4 KB steps lost up to 4 KB of it */
    if (avail > s_sys_cfg.max_heap) avail = s_sys_cfg.max_heap; /* [memory] max_heap */
    if (avail < 16384) avail = 16384;
    ESP_LOGI(TAG, "Memory Probe: free=%u B, largest=%u B, reserved=%u B => VM heap %u KB",
             (unsigned)free_heap, (unsigned)largest_8bit, (unsigned)reserved, (unsigned)(avail / 1024));
    s_probe.free_heap = (uint32_t)free_heap;
    s_probe.largest = (uint32_t)largest_8bit;
    s_probe.wifi = esp32_wifi_connected();
    s_probe.reserved = (uint32_t)reserved;

    s_vm = create_app_vm(avail);
    if (!s_vm) {
        ESP_LOGE(TAG, "Failed to initialize MiniVM!");
        return;
    }

    /* 7. Run the app in place from Flash, else load it from SPIFFS to RAM
     * (bytecode, or a heap snapshot restored in place of the whole heap),
     * falling back to the copy embedded in the firmware */
    Value app_root = rom_ok ? mini_rom_attach(s_vm, s_rom_ptr, s_rom_part->size, rom_key) : mini_void();
    /* the app's own code touches only its folder; the settings are the system */
    mini_app_sandbox_set(s_settings_mode ? "" : s_app_dir);
    s_app_quota = (size_t)(s_app_storage_kb > s_sys_cfg.app_storage_kb ? s_app_storage_kb : s_sys_cfg.app_storage_kb) * 1024;
    mini_app_set_storage(s_app_quota, dir_used_bytes);
    market_host_start(s_settings_mode, market_auto_update, builtin_app);
    {
        uint32_t market_auto = s_market_auto;
        s_market_auto = 0;
        if (s_settings_mode && market_auto == MARKET_AUTO_UPDATE_MAGIC) {
            s_market_auto = MARKET_AUTO_SKIP_MAGIC;   /* the app boot after this round checks nothing */
            mini_market_auto_start(MINI_MARKET_AUTO_UPDATE);
        } else if (!s_settings_mode && market_auto != MARKET_AUTO_SKIP_MAGIC) {
            mini_market_auto_start(MINI_MARKET_AUTO_CHECK);   /* market_auto_update = 1 only */
        }
    }
    if (s_settings_mode) {
        /* the settings from the market when valid, else the built-in bootstrap */
        bool ran = false;
        char *meta = file_has_magic(SETTINGS_APP_PATH, "MSBC") ? mini_app_meta_read_file(SETTINGS_APP_PATH) : NULL;
        char why[112];
        if (s_builtin_settings) {
            ESP_LOGI(TAG, "Built-in system settings (after a factory reset)");
        } else if (meta && mini_market_use_installed("settings", meta, app_settings_msbc_data, APP_SETTINGS_MSBC_LEN,
                                                     why, sizeof(why))) {
            execute_path(s_vm, SETTINGS_APP_PATH);
            if (mini_get_error(s_vm)) {
                ESP_LOGE(TAG, "%s failed: %s; using the built-in settings", SETTINGS_APP_PATH, mini_get_error(s_vm));
                mini_clear_error(s_vm);
            } else {
                ESP_LOGI(TAG, "System settings from %s", SETTINGS_APP_PATH);
                s_settings_from = "market";
                ran = true;
            }
        } else if (meta) {
            ESP_LOGW(TAG, "%s not used: %s", SETTINGS_APP_PATH, why);
        }
        free(meta);
        if (!ran) execute_msbc_chunk(s_vm, app_settings_msbc_data, APP_SETTINGS_MSBC_LEN);
    } else if (mini_is_code(app_root)) {
        ESP_LOGI(TAG, "App runs in place from Flash (%u B image)", (unsigned)mini_rom_size(s_vm));
        run_code_value(s_vm, app_root);
    } else {
        execute_path(s_vm, app_path);
        if (fasl_app && !mini_get_error(s_vm)) {
            ESP_LOGI(TAG, "App restored from heap snapshot %s", app_path);
        }
    }
    if (mini_get_error(s_vm) && !s_settings_mode) {
        char why[120];
        snprintf(why, sizeof(why), "%s", mini_get_error(s_vm));
        mini_clear_error(s_vm);
        app_failed("load", why);
    }
    if (mini_get_error(s_vm)) {
        ESP_LOGE(TAG, "App load from %s failed: %s; using built-in settings", app_path, mini_get_error(s_vm));
        mini_clear_error(s_vm);
        if (fasl_app) {
            /* a failed snapshot restore may have emptied the heap: start over */
            mini_destroy(s_vm);
            s_vm = create_app_vm(avail);
            if (!s_vm) {
                ESP_LOGE(TAG, "Failed to initialize MiniVM!");
                return;
            }
        }
        execute_msbc_chunk(s_vm, app_settings_msbc_data, APP_SETTINGS_MSBC_LEN);
    }
    if (mini_get_error(s_vm)) {
        ESP_LOGE(TAG, "Application load error: %s", mini_get_error(s_vm));
        mini_clear_error(s_vm);
    }
    mini_gc(s_vm);
    mini_set_baseline_env(s_vm);
    ESP_LOGI(TAG, "Application loaded: VM heap %u / %u B, system free %u B",
             (unsigned)mini_get_allocated_bytes(s_vm), (unsigned)mini_get_heap_size(s_vm),
             (unsigned)esp_get_free_heap_size());

#if MINISCHEME_FW_COMPILER
    run_code_value(s_vm, msbc_deserialize(s_vm, compiler_msbc_data, compiler_msbc_len));
    if (mini_get_error(s_vm)) {
        ESP_LOGW(TAG, "On-device compiler unavailable: %s", mini_get_error(s_vm));
        mini_clear_error(s_vm);
    } else {
        s_has_compiler = true;
        mini_gc(s_vm);
    }
#endif

    /* 8. Serial console */
    const uart_config_t uart_cfg = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(UART_NUM_0, 2048, 512, 0, NULL, 0);
    uart_param_config(UART_NUM_0, &uart_cfg);
    uart_vfs_dev_use_driver(UART_NUM_0);

    printf("\r\n====================================================\r\n");
    printf("  MiniScheme REPL - ESP32 3.5\" ILI9488 (320x480)\r\n");
    printf("  Compiler on device: %s\r\n", s_has_compiler ? "yes (text REPL enabled)" : "no (use WebREPL bytecode mode)");
    printf("  Remote protocol v%d: @INFO / @EXEC / @BEGIN @DATA @END\r\n", REMOTE_PROTO_VERSION);
    printf("====================================================\r\n");
    printf("@READY {\"proto\":%d,\"compiler\":%s}\r\n", REMOTE_PROTO_VERSION, s_has_compiler ? "true" : "false");
    printf("minischeme> ");
    fflush(stdout);

    mini_gui_invalidate(0, s_sys_cfg.height);
    service_display();

    uint32_t remote_presses_seen = s_remote_presses;
    uint32_t hw_presses_seen = esp32_button_presses();
    int last_clock_sec = -1;
    int last_lux = -1;
    int last_bl = 220;

    s_stats_t0 = esp_timer_get_time();
    while (1) {
        int64_t loop_t0 = esp_timer_get_time();
        /* Remote requests */
        serial_poll();
        service_pending_job();

        /* Physical button (GPIO 18), or the WebREPL's: short, long and mid
         * press for the app, the hold the hardware pages, a mid press there
         * the system settings */
        uint32_t presses = s_remote_presses;
        bool remote_tap = presses != remote_presses_seen;
        remote_presses_seen = presses;
        bool remote_held = s_remote_btn && loop_t0 < s_remote_btn_until;
        /* a press that began and ended since the last pass (a slow frame)
         * counts as pressed for this pass, like a remote tap */
        uint32_t hw_presses = esp32_button_presses();
        bool hw_level = esp32_button_read();
        bool hw_tap = hw_presses != hw_presses_seen && !hw_level;
        hw_presses_seen = hw_presses;
        bool btn_pressed = hw_level || hw_tap || remote_held || remote_tap;
        mini_button_set(btn_pressed);
        mini_clear_error(s_vm);   /* nothing left over from a REPL job */
        MiniButtonEvent btn_ev = mini_device_button_update(s_vm, btn_pressed, (uint32_t)(loop_t0 / 1000));
        check_app_error("button");
        if (btn_ev == MINI_BUTTON_SYSTEM) open_system_settings();

        mini_device_call_hook(s_vm, "app-step", NULL, 0, NULL);
        check_app_error("app-step");
        backlight_save_pending();
        /* the app market: downloads in progress; the automatic update round */
        mini_market_poll();
        if (!s_settings_restart_at) mini_market_auto((uint32_t)(loop_t0 / 1000));
        if (s_factory_restart_at && esp_timer_get_time() >= s_factory_restart_at) {
            ESP_LOGW(TAG, "Restarting into the built-in settings after a factory reset");
            esp_restart();   /* the RTC flags select the built-in settings */
        }
        if (s_settings_restart_at && esp_timer_get_time() >= s_settings_restart_at) {
            s_settings_restart_at = 0;
            open_system_settings();
        }

        /* 1 Hz tick keyed on the displayed clock: the wall clock once NTP has
         * synced (uptime before that), so render latency can never make the
         * shown second repeat or skip */
        time_t wall_now = time(NULL);
        int now_sec = hal_time_synced() ? (int)wall_now : (int)(esp_timer_get_time() / 1000000LL);
        if (s_light_dirty) { /* the WebREPL's light value, or the sensor again */
            s_light_dirty = false;
            int light = s_light_override;
            int raw_adc = light >= 0 ? light : read_light_filtered();
            mini_light_set(raw_adc);
            last_lux = (raw_adc * 1000) / 4095;
        }
        if (now_sec != last_clock_sec) {
            last_clock_sec = now_sec;
            if (now_sec % 3 == 0 || last_lux < 0) {
                int light = s_light_override;
                int raw_adc = light >= 0 ? light : read_light_filtered();
                mini_light_set(raw_adc);
                last_lux = (raw_adc * 1000) / 4095;
                last_bl = mini_backlight_get();
            }
            MiniWallTime wt;
            read_wall_time(&wt, wall_now);
            /* Seconds of day keeps the tick a small fixnum on 32-bit targets */
            mini_device_clock_tick(s_vm, &wt, wt.synced ? (int)(wall_now % 86400) : now_sec, last_lux, last_bl);
            if (mini_get_error(s_vm)) ESP_LOGE(TAG, "app-tick error: %s", mini_get_error(s_vm));
            check_app_error("app-tick");
            if (now_sec % 60 == 0) {
                ESP_LOGI(TAG, "Clock %s | heap %u/%u B | free %u B",
                         wt.synced ? "SYNCED" : "WAITING NTP",
                         (unsigned)mini_get_allocated_bytes(s_vm), (unsigned)mini_get_heap_size(s_vm),
                         (unsigned)esp_get_free_heap_size());
            }
        }

        mini_gui_animate((uint32_t)(esp_timer_get_time() / 1000)); /* scrolling text */
        service_display();

        int64_t loop_t1 = esp_timer_get_time();
        s_busy_acc += loop_t1 - loop_t0;
        if (loop_t1 - s_stats_t0 >= 1000000) {
            int64_t span = loop_t1 - s_stats_t0;
            s_loop.busy_pct = (int)(s_busy_acc * 100 / span);
            s_loop.fps_x10 = (int)((int64_t)s_frames_acc * 10000000 / span);
            s_loop.rows_per_s = (int)((int64_t)s_rows_acc * 1000000 / span);
            s_busy_acc = 0;
            s_frames_acc = s_rows_acc = 0;
            s_stats_t0 = loop_t1;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
