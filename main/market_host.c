/**
 * @file market_host.c
 * @brief The board's side of the app market (minischeme_market.c): a
 *        background HTTP task that streams a URL to a file or to memory,
 *        and the file and config hooks on LittleFS (/spiffs).
 *
 * One fetch at a time. The task only writes its own buffer or file and a few
 * counters; the engine reads them from the main loop (fetch_poll).
 */

#include "market_host.h"
#include "minischeme_market.h"
#include "minischeme_device.h"
#include "minischeme_channel.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "market";

#define FETCH_STACK 4096
#define FETCH_CHUNK 512

static struct {
    char url[240];
    char file[48];          /* "" : into memory */
    size_t max;
    volatile int state;     /* 0 running, 1 done, -1 failed, 2 idle */
    volatile size_t received, total;
    volatile bool cancel;
    char err[64];
    char *buf;              /* memory result */
    size_t len;
} F = { .state = 2 };

static void fetch_task(void *arg) {
    (void)arg;
    FILE *fp = NULL;
    char *chunk = (char *)malloc(FETCH_CHUNK);
    size_t cap = 0;             /* of F.buf */
    int result = -1;
    int status = 0;
    int64_t length = -1;
    MiniFetch *c = NULL;
    if (!chunk) {
        snprintf(F.err, sizeof(F.err), "out of memory");
        goto done;
    }
    /* plain HTTP, or through the encrypted channel (config.ini [channel] market_via_channel) */
    c = mini_fetch_open(F.url, 10000, MINI_FETCH_REDIRECTS | MINI_FETCH_BIG, &status, &length, F.err, sizeof(F.err));
    if (!c) goto done;
    if (status != 200) {
        snprintf(F.err, sizeof(F.err), "HTTP %d", status);
        goto done;
    }
    F.total = length > 0 ? (size_t)length : 0;
    if (F.file[0]) {
        fp = fopen(F.file, "wb");
        if (!fp) {
            snprintf(F.err, sizeof(F.err), "cannot write %.30s (errno %d)", F.file, errno);
            goto done;
        }
    } else {
        cap = F.total && F.total < F.max ? F.total + 1 : 2048;
        F.buf = (char *)malloc(cap);
        if (!F.buf) {
            snprintf(F.err, sizeof(F.err), "out of memory");
            goto done;
        }
    }
    for (;;) {
        if (F.cancel) {
            snprintf(F.err, sizeof(F.err), "cancelled");
            goto done;
        }
        int n = mini_fetch_read(c, chunk, FETCH_CHUNK);
        if (n < 0) {
            snprintf(F.err, sizeof(F.err), "read failed");
            goto done;
        }
        if (n == 0) break;
        if (fp) {
            if (fwrite(chunk, 1, (size_t)n, fp) != (size_t)n) {
                snprintf(F.err, sizeof(F.err), errno == ENOSPC ? "file system full" : "write failed (errno %d)", errno);
                goto done;
            }
        } else {
            if (F.len + (size_t)n + 1 > cap) {
                size_t want = cap * 2 > F.len + (size_t)n + 1 ? cap * 2 : F.len + (size_t)n + 1;
                if (want > F.max + 1) {
                    snprintf(F.err, sizeof(F.err), "larger than %u bytes", (unsigned)F.max);
                    goto done;
                }
                char *nb = (char *)realloc(F.buf, want);
                if (!nb) {
                    snprintf(F.err, sizeof(F.err), "out of memory");
                    goto done;
                }
                F.buf = nb;
                cap = want;
            }
            memcpy(F.buf + F.len, chunk, (size_t)n);
            F.len += (size_t)n;
            F.buf[F.len] = '\0';
        }
        F.received += (size_t)n;
    }
    if (F.total && F.received != F.total) {
        snprintf(F.err, sizeof(F.err), "incomplete (%u of %u bytes)", (unsigned)F.received, (unsigned)F.total);
        goto done;
    }
    result = 1;
done:
    if (fp && fclose(fp) != 0 && result == 1) {
        snprintf(F.err, sizeof(F.err), errno == ENOSPC ? "file system full" : "write failed (errno %d)", errno);
        result = -1;
    }
    if (c) {
        mini_fetch_close(c);
    }
    free(chunk);
    if (result < 0 && F.buf) {
        free(F.buf);
        F.buf = NULL;
        F.len = 0;
    }
    F.state = result;
    vTaskDelete(NULL);
}

static bool host_fetch_start(const char *url, const char *file, size_t max) {
    if (F.state == 0) return false;
    free(F.buf);
    F.buf = NULL;
    F.len = 0;
    snprintf(F.url, sizeof(F.url), "%s", url);
    snprintf(F.file, sizeof(F.file), "%s", file ? file : "");
    F.max = max ? max : 64 * 1024;
    F.received = F.total = 0;
    F.cancel = false;
    F.err[0] = '\0';
    F.state = 0;
    if (xTaskCreate(fetch_task, "market", FETCH_STACK, NULL, 4, NULL) != pdPASS) {
        F.state = 2;
        return false;
    }
    return true;
}

static int host_fetch_poll(size_t *received, size_t *total, const char **err) {
    *received = F.received;
    *total = F.total;
    *err = F.err;
    return F.state == 2 ? -1 : F.state;
}

static const char *host_fetch_data(size_t *len) {
    *len = F.len;
    return F.buf;
}

static void host_fetch_release(void) {
    free(F.buf);
    F.buf = NULL;
    F.len = 0;
    if (F.state != 0) F.state = 2;
}

static void host_fetch_cancel(void) {
    if (F.state != 0) return;
    F.cancel = true;
    for (int i = 0; i < 150 && F.state == 0; i++) vTaskDelay(pdMS_TO_TICKS(20));   /* the task ends at the next chunk */
    host_fetch_release();
}

static bool host_file_hash(const char *path, size_t *size, uint32_t *hash, uint8_t head[4]) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;
    uint8_t buf[256];
    uint32_t h = 2166136261u;
    size_t total = 0, n;
    memset(head, 0, 4);
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        if (total < 4) memcpy(head + total, buf, n < 4 - total ? n : 4 - total);
        for (size_t i = 0; i < n; i++) h = (h ^ buf[i]) * 16777619u;
        total += n;
    }
    fclose(fp);
    *size = total;
    *hash = h;
    return true;
}

static char *host_file_meta(const char *path) { return mini_app_meta_read_file(path); }

static bool host_file_replace(const char *tmp, const char *path) {
    remove(path);
    return rename(tmp, path) == 0;
}

static void host_file_remove(const char *path) { remove(path); }

static bool host_config(const char *key, char *out, size_t cap) {
    FILE *fp = fopen("/spiffs/config.ini", "rb");
    out[0] = '\0';
    if (!fp) return false;
    char *text = (char *)malloc(8192);
    size_t n = text ? fread(text, 1, 8191, fp) : 0;
    fclose(fp);
    if (!text) return false;
    text[n] = '\0';
    bool ok = mini_device_config_value(text, key, out, cap);
    free(text);
    return ok;
}

static bool host_online(void) {
    esp_netif_ip_info_t ip;
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    return sta && esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr != 0;
}

static void host_log(const char *msg) { ESP_LOGI(TAG, "%s", msg); }

/* Installs only in the system settings: an app's VM leaves the system heap
 * too short for a download (HTTP buffers, LittleFS cache, a 4 KB task) */
static bool s_settings;

static bool host_install_here(void) { return s_settings; }

static void (*s_auto_update)(void);

static void host_auto_update(void) {
    if (s_auto_update) s_auto_update();
}

static const uint8_t *(*s_builtin)(const char *id, size_t *len);

static const uint8_t *host_builtin(const char *id, size_t *len) {
    return s_builtin ? s_builtin(id, len) : NULL;
}

static const MiniMarketHost HOST = {
    host_fetch_start, host_fetch_poll, host_fetch_data, host_fetch_release, host_fetch_cancel,
    host_file_hash, host_file_meta, host_file_replace, host_file_remove, host_config, host_online, host_log,
    host_install_here, host_auto_update, host_builtin,
};

void market_host_init(void) { mini_market_set_host(&HOST); }

void market_host_start(bool settings_mode, void (*auto_update)(void),
                       const uint8_t *(*builtin)(const char *id, size_t *len)) {
    s_settings = settings_mode;
    s_auto_update = auto_update;
    s_builtin = builtin;
}
