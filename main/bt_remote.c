/**
 * @file bt_remote.c
 * @brief A Bluetooth LE HID remote as a second key (see bt_remote.h).
 *
 * Tasks: the NimBLE host task (nimble_port_run), esp_hidh's event task (the
 * remote's reports, decoded here into keys and queued) and bt_remote, which
 * scans, pairs and reconnects. The main loop takes the keys with
 * bt_remote_poll(): only it touches the VM.
 */

#include "bt_remote.h"

#include <stdio.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_hidh.h"
#include "esp_hidh_nimble.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs.h"

void ble_store_config_init(void);   /* NimBLE's NVS bond store (no header) */

static const char *TAG = "BtRemote";

#define BT_NVS_NS        "bt_remote"
#define BT_SCAN_MS       10000      /* one scan for a remote in pairing mode */
#define BT_SCAN_PAUSE_MS 20000      /* between scans: WiFi shares the radio */
#define BT_RETRY_MS      3000       /* between reconnect attempts */
#define BT_SEEN_MAX      6
#define HID_UUID16       0x1812

typedef enum { ST_OFF, ST_STARTING, ST_SCANNING, ST_CONNECTING, ST_CONNECTED, ST_WAITING } BtState;
static const char *const STATE_NAMES[] = { "off", "starting", "scanning", "connecting", "connected", "waiting" };

typedef struct {
    uint8_t bda[6];
    uint8_t type;
    int8_t rssi;
    bool hid;
    char name[24];
} BtSeen;

static QueueHandle_t s_keys;
static SemaphoreHandle_t s_wake;      /* scan over, remote gone, forget */
static SemaphoreHandle_t s_synced;    /* the host is ready */
static SemaphoreHandle_t s_lock;      /* the status fields below */
static volatile BtState s_state = ST_OFF;
static bool s_have_bda, s_bonded;
static uint8_t s_bda[6], s_bda_type;
static char s_name[32];
static int s_battery = -1;
static char s_last[40];
static BtSeen s_seen[BT_SEEN_MAX];
static int s_nseen;
static BtSeen s_cand;                  /* the strongest HID device of a scan */
static bool s_cand_found;
static volatile bool s_forget;
static uint8_t s_kbd_key, s_cc_down;   /* the key held: a press is reported once */

static void lock(void)   { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }

static void bda_str(const uint8_t *b, char *out, size_t cap) {
    /* NimBLE keeps addresses little-endian: printed most significant first */
    snprintf(out, cap, "%02x:%02x:%02x:%02x:%02x:%02x", b[5], b[4], b[3], b[2], b[1], b[0]);
}

void bt_remote_release_memory(void) {
    esp_err_t e = esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) ESP_LOGW(TAG, "controller memory release: %s", esp_err_to_name(e));
}

/* ---- the paired remote in NVS ------------------------------------------- */

static void nvs_load(void) {
    nvs_handle_t h;
    if (nvs_open(BT_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t blob[7];
    size_t n = sizeof(blob);
    if (nvs_get_blob(h, "bda", blob, &n) == ESP_OK && n == sizeof(blob)) {
        memcpy(s_bda, blob, 6);
        s_bda_type = blob[6];
        s_have_bda = s_bonded = true;
        n = sizeof(s_name);
        if (nvs_get_str(h, "name", s_name, &n) != ESP_OK) s_name[0] = '\0';
    }
    nvs_close(h);
}

static void nvs_save(void) {
    nvs_handle_t h;
    if (nvs_open(BT_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    uint8_t blob[7];
    memcpy(blob, s_bda, 6);
    blob[6] = s_bda_type;
    nvs_set_blob(h, "bda", blob, sizeof(blob));
    nvs_set_str(h, "name", s_name);
    nvs_commit(h);
    nvs_close(h);
}

static void nvs_erase(void) {
    nvs_handle_t h;
    if (nvs_open(BT_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
}

/* ---- keys from the remote's reports ---------------------------------------- */

static void push_key(BtRemoteKey k, const char *what) {
    lock();
    snprintf(s_last, sizeof(s_last), "%s (%s)", bt_remote_key_name(k), what);
    unlock();
    if (s_keys) xQueueSend(s_keys, &k, 0);
}

static BtRemoteKey keyboard_key(uint8_t code) {
    switch (code) {
    case 0x4F: case 0x51: case 0x4E: case 0x2C: case 0x81: return BT_KEY_NEXT;   /* right down pgdn space vol- */
    case 0x50: case 0x52: case 0x4B: case 0x80:            return BT_KEY_PREV;   /* left up pgup vol+ */
    case 0x28: case 0x58:                                  return BT_KEY_OK;     /* enter, keypad enter */
    case 0x29:                                             return BT_KEY_BACK;   /* escape */
    default:                                               return BT_KEY_NONE;
    }
}

static BtRemoteKey consumer_key(uint16_t usage) {
    switch (usage) {
    case 0xEA: case 0xB5: return BT_KEY_NEXT;    /* volume down, next track */
    case 0xE9: case 0xB6: return BT_KEY_PREV;    /* volume up, previous track */
    case 0xCD: case 0x40: return BT_KEY_OK;      /* play / pause, menu */
    case 0x224:           return BT_KEY_BACK;    /* AC back */
    default:              return BT_KEY_NONE;
    }
}

/* A keyboard report: modifiers, reserved, then up to 6 key codes (some
 * remotes leave out the reserved byte); a key counts when it goes down */
static void keyboard_report(const uint8_t *d, size_t n) {
    size_t from = n >= 8 ? 2 : 1;
    uint8_t code = 0;
    for (size_t i = from; i < n; i++) {
        if (d[i]) { code = d[i]; break; }
    }
    if (code && code != s_kbd_key) {
        char what[16];
        snprintf(what, sizeof(what), "key 0x%02x", code);
        BtRemoteKey k = keyboard_key(code);
        if (k != BT_KEY_NONE) push_key(k, what);
        else { lock(); snprintf(s_last, sizeof(s_last), "unmapped %s", what); unlock(); }
    }
    s_kbd_key = code;
}

static void consumer_report(const uint8_t *d, size_t n) {
    uint16_t usage = n >= 2 ? (uint16_t)(d[0] | (d[1] << 8)) : (n ? d[0] : 0);
    if (usage && !s_cc_down) {
        char what[20];
        snprintf(what, sizeof(what), "usage 0x%03x", usage);
        BtRemoteKey k = consumer_key(usage);
        if (k != BT_KEY_NONE) push_key(k, what);
        else { lock(); snprintf(s_last, sizeof(s_last), "unmapped %s", what); unlock(); }
    }
    s_cc_down = usage != 0;
}

static void hidh_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base;
    esp_hidh_event_data_t *p = (esp_hidh_event_data_t *)data;
    switch ((esp_hidh_event_t)id) {
    case ESP_HIDH_OPEN_EVENT:
        if (p->open.status == ESP_OK) {
            const char *name = esp_hidh_dev_name_get(p->open.dev);
            lock();
            snprintf(s_name, sizeof(s_name), "%s", name ? name : "");
            unlock();
            ESP_LOGI(TAG, "Remote connected: %s", s_name);
            nvs_save();
            s_bonded = true;
        }
        break;
    case ESP_HIDH_BATTERY_EVENT:
        s_battery = p->battery.level;
        break;
    case ESP_HIDH_INPUT_EVENT: {
        const uint8_t *d = p->input.data;
        size_t n = p->input.length;
        char hex[3 * 12 + 1] = "";
        for (size_t i = 0; i < n && i < 12; i++) snprintf(hex + 3 * i, 4, "%02x ", d[i]);
        ESP_LOGI(TAG, "Report: %s id %u, %u B: %s", esp_hid_usage_str(p->input.usage),
                 (unsigned)p->input.report_id, (unsigned)n, hex);
        if (p->input.usage == ESP_HID_USAGE_KEYBOARD) keyboard_report(d, n);
        else if (p->input.usage == ESP_HID_USAGE_CCONTROL) consumer_report(d, n);
        else { lock(); snprintf(s_last, sizeof(s_last), "%s report", esp_hid_usage_str(p->input.usage)); unlock(); }
        break;
    }
    case ESP_HIDH_CLOSE_EVENT:
        ESP_LOGI(TAG, "Remote disconnected");
        s_state = ST_WAITING;
        s_kbd_key = s_cc_down = 0;
        xSemaphoreGive(s_wake);
        break;
    default:
        break;
    }
}

/* ---- scanning ------------------------------------------------------------------ */

static bool adv_is_hid(const struct ble_hs_adv_fields *f) {
    for (int i = 0; i < f->num_uuids16; i++) {
        if (ble_uuid_u16(&f->uuids16[i].u) == HID_UUID16) return true;
    }
    if (f->appearance_is_present) {
        uint16_t cat = f->appearance >> 6;            /* 0x0F: HID, 0x06: remote control */
        if (cat == 0x0F || cat == 0x06) return true;
    }
    return false;
}

static void scan_result(const struct ble_gap_disc_desc *d) {
    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, d->data, d->length_data) != 0) return;
    bool hid = adv_is_hid(&f);
    lock();
    int i = 0;
    while (i < s_nseen && memcmp(s_seen[i].bda, d->addr.val, 6) != 0) i++;
    if (i == s_nseen && s_nseen < BT_SEEN_MAX && (hid || f.name_len)) s_nseen++;
    if (i < s_nseen) {
        BtSeen *s = &s_seen[i];
        memcpy(s->bda, d->addr.val, 6);
        s->type = d->addr.type;
        s->rssi = d->rssi;
        s->hid = s->hid || hid;
        if (f.name_len) {
            size_t n = f.name_len < sizeof(s->name) - 1 ? f.name_len : sizeof(s->name) - 1;
            memcpy(s->name, f.name, n);
            s->name[n] = '\0';
        }
        if (s->hid && (!s_cand_found || s->rssi > s_cand.rssi || memcmp(s_cand.bda, s->bda, 6) == 0)) {
            s_cand = *s;
            s_cand_found = true;
        }
    }
    unlock();
}

static int gap_event(struct ble_gap_event *ev, void *arg) {
    (void)arg;
    if (ev->type == BLE_GAP_EVENT_DISC) scan_result(&ev->disc);
    else if (ev->type == BLE_GAP_EVENT_DISC_COMPLETE) xSemaphoreGive(s_wake);
    return 0;
}

static bool scan(void) {
    uint8_t own;
    if (ble_hs_id_infer_auto(0, &own) != 0) return false;
    struct ble_gap_disc_params params;
    memset(&params, 0, sizeof(params));
    params.filter_duplicates = 1;       /* active scan: names come in the scan responses */
    lock();
    s_nseen = 0;
    s_cand_found = false;
    unlock();
    xSemaphoreTake(s_wake, 0);
    int rc = ble_gap_disc(own, BT_SCAN_MS, &params, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "scan failed: %d", rc);
        return false;
    }
    xSemaphoreTake(s_wake, pdMS_TO_TICKS(BT_SCAN_MS + 3000));
    return true;
}

/* ---- the connection task ------------------------------------------------------- */

static void forget_now(void) {
    s_forget = false;
    if (s_have_bda) {
        ble_addr_t a = { .type = s_bda_type };
        memcpy(a.val, s_bda, 6);
        ble_store_util_delete_peer(&a);
    }
    nvs_erase();
    lock();
    s_have_bda = s_bonded = false;
    s_name[0] = '\0';
    s_battery = -1;
    unlock();
    ESP_LOGI(TAG, "Paired remote forgotten");
}

static void remote_task(void *arg) {
    (void)arg;
    xSemaphoreTake(s_synced, portMAX_DELAY);
    ESP_LOGI(TAG, "Bluetooth ready, %s", s_have_bda ? "reconnecting to the paired remote" : "looking for a remote");
    for (;;) {
        if (s_forget) forget_now();
        if (s_have_bda) {
            s_state = ST_CONNECTING;
            /* blocks until connected and discovered, or 30 s: a sleeping
             * remote advertises again when a key is pressed */
            esp_hidh_dev_t *dev = esp_hidh_dev_open(s_bda, ESP_HID_TRANSPORT_BLE, s_bda_type);
            if (dev) {
                s_state = ST_CONNECTED;
                xSemaphoreTake(s_wake, portMAX_DELAY);   /* until it goes away (or forget) */
                if (s_forget) esp_hidh_dev_close(dev);
                continue;
            }
            if (!s_bonded) {   /* a scan's pick that never connected: look again */
                lock();
                s_have_bda = false;
                unlock();
            }
            s_state = ST_WAITING;
            xSemaphoreTake(s_wake, pdMS_TO_TICKS(BT_RETRY_MS));
            continue;
        }
        s_state = ST_SCANNING;
        if (scan() && s_cand_found) {
            char a[18];
            bda_str(s_cand.bda, a, sizeof(a));
            ESP_LOGI(TAG, "Pairing with %s (%s, %d dBm)", s_cand.name, a, s_cand.rssi);
            lock();
            memcpy(s_bda, s_cand.bda, 6);
            s_bda_type = s_cand.type;
            snprintf(s_name, sizeof(s_name), "%s", s_cand.name);
            s_have_bda = true;
            unlock();
            continue;
        }
        s_state = ST_WAITING;
        xSemaphoreTake(s_wake, pdMS_TO_TICKS(BT_SCAN_PAUSE_MS));
    }
}

static void host_task(void *arg) {
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void host_synced(void) {
    xSemaphoreGive(s_synced);
}

bool bt_remote_start(void) {
    s_keys = xQueueCreate(8, sizeof(BtRemoteKey));
    s_wake = xSemaphoreCreateBinary();
    s_synced = xSemaphoreCreateBinary();
    s_lock = xSemaphoreCreateMutex();
    if (!s_keys || !s_wake || !s_synced || !s_lock) return false;
    s_state = ST_STARTING;
    nvs_load();
    /* only BLE is used: the classic controller's memory goes to the heap */
    esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    esp_err_t e = nimble_port_init();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(e));
        s_state = ST_OFF;
        return false;
    }
    ble_hs_cfg.sync_cb = host_synced;     /* esp_hidh_init chains it */
    esp_hidh_config_t cfg = { .callback = hidh_event, .event_stack_size = 3072, .callback_arg = NULL };
    e = esp_hidh_init(&cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_hidh_init: %s", esp_err_to_name(e));
        s_state = ST_OFF;
        return false;
    }
    /* a remote has no display and no keypad: "just works" pairing, bonded */
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_store_config_init();
    nimble_port_freertos_init(host_task);
    if (xTaskCreate(remote_task, "bt_remote", 3072, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "no memory for the task");
        return false;
    }
    return true;
}

BtRemoteKey bt_remote_poll(void) {
    BtRemoteKey k = BT_KEY_NONE;
    if (s_keys) xQueueReceive(s_keys, &k, 0);
    return k;
}

void bt_remote_forget(void) {
    if (s_state == ST_OFF) {
        nvs_erase();
        return;
    }
    s_forget = true;
    xSemaphoreGive(s_wake);
}

const char *bt_remote_key_name(BtRemoteKey key) {
    switch (key) {
    case BT_KEY_NEXT: return "next";
    case BT_KEY_PREV: return "prev";
    case BT_KEY_OK:   return "ok";
    case BT_KEY_BACK: return "back";
    default:          return "";
    }
}

/* JSON string body: quotes, backslashes and control bytes left out */
static void json_clean(const char *in, char *out, size_t cap) {
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 1 < cap; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c >= 0x20 && c != '"' && c != '\\') out[j++] = (char)c;
    }
    out[j] = '\0';
}

size_t bt_remote_status_json(char *out, size_t cap) {
    char name[32], last[40], addr[18] = "";
    lock();
    json_clean(s_name, name, sizeof(name));
    json_clean(s_last, last, sizeof(last));
    if (s_have_bda) bda_str(s_bda, addr, sizeof(addr));
    int n = snprintf(out, cap, "{\"enabled\":%s,\"state\":\"%s\",\"paired\":%s,\"name\":\"%s\",\"address\":\"%s\","
                     "\"battery\":%d,\"last\":\"%s\",\"seen\":[",
                     s_state != ST_OFF ? "true" : "false", STATE_NAMES[s_state], s_bonded ? "true" : "false",
                     name, addr, s_battery, last);
    for (int i = 0; i < s_nseen && n > 0 && (size_t)n < cap; i++) {
        char sn[24], sa[18];
        json_clean(s_seen[i].name, sn, sizeof(sn));
        bda_str(s_seen[i].bda, sa, sizeof(sa));
        n += snprintf(out + n, cap - (size_t)n, "%s{\"name\":\"%s\",\"address\":\"%s\",\"rssi\":%d,\"hid\":%s}",
                      i ? "," : "", sn, sa, s_seen[i].rssi, s_seen[i].hid ? "true" : "false");
    }
    if (n > 0 && (size_t)n < cap) n += snprintf(out + n, cap - (size_t)n, "]}");
    unlock();
    return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}
