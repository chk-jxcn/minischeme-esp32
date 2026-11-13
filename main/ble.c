/**
 * @file ble.c
 * @brief Bluetooth Low Energy for apps: the `ble` API extension on the NimBLE host.
 *
 * Roles: observer (scan), central (connect to a peripheral: discover, read, write,
 * subscribe), broadcaster and peripheral (advertise; the Nordic UART service for text to
 * and from a phone). Nothing blocks: a procedure starts an operation and its outcome
 * arrives as an event, taken with (ble-event) or handed to the app's (app-ble-event ev).
 *
 * The host task only fills the tables below under s_lock; the VM (main loop) reads them.
 * All memory of the tables is allocated when the stack starts: nothing while it is off.
 */

#include "ble.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "minischeme_device.h"
#include "minischeme_internal.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "ble";

#define BLE_SCAN_MAX      12      /* devices the scan table keeps (the weakest leaves) */
#define BLE_EVENT_MAX     10      /* queued events (the oldest leaves) */
#define BLE_EVENT_DATA    128     /* bytes of an event's value */
#define BLE_CONN_MAX      CONFIG_BT_NIMBLE_MAX_CONNECTIONS
#define BLE_SVC_MAX       10      /* services kept of a peer */
#define BLE_CHR_MAX       40     /* characteristics kept of a peer */
#define BLE_NAME_MAX      24
#ifdef CONFIG_BT_NIMBLE_SECURITY_ENABLE
#define BLE_SECURITY 1          /* pairing, bonds (in NVS), encrypted links */
#else
#define BLE_SECURITY 0
#define CONFIG_BT_NIMBLE_MAX_BONDS 1
#endif

typedef enum {
    EV_CONNECTED = 1, EV_DISCONNECTED, EV_DISCOVERED, EV_READ, EV_WRITTEN, EV_SUBSCRIBED,
    EV_NOTIFY, EV_UART, EV_SCAN_DONE, EV_PAIRED,
} BleEventType;

typedef struct {
    uint8_t type;
    uint8_t len;
    uint16_t conn, handle;
    int16_t status;
    uint8_t data[BLE_EVENT_DATA];
} BleEvent;

typedef struct {
    ble_addr_t addr;
    int8_t rssi;
    bool connectable;
    uint8_t adv_len, rsp_len;
    char name[BLE_NAME_MAX + 1];
    uint8_t adv[31], rsp[31];
    int64_t seen_us;
} BleSeen;

typedef struct {
    uint16_t start, end;
    ble_uuid_any_t uuid;
} BleSvc;

typedef struct {
    uint16_t def_handle, val_handle;
    uint8_t props;
    ble_uuid_any_t uuid;
} BleChr;

typedef struct {
    bool used, central, encrypted;
    uint8_t discover;            /* 0 not asked, 1 services, 2 characteristics, 3 done, 4 failed */
    uint16_t handle, mtu;
    ble_addr_t peer;
    uint8_t nsvc, nchr;
    BleSvc svc[BLE_SVC_MAX];
    BleChr *chr;                 /* BLE_CHR_MAX, allocated by a discovery */
    uint16_t sub_handle;         /* the characteristic a subscription is looking up the CCCD of */
    bool sub_on, sub_found;
} BleConn;

static SemaphoreHandle_t s_lock;
static bool s_running, s_synced, s_scanning, s_advertising, s_connecting;
static uint8_t s_own_addr_type;
static size_t s_heap_used;
static BleEvent *s_events;
static int s_ev_head, s_ev_count;
static BleSeen *s_seen;
static int s_seen_count;
static BleConn s_conn[BLE_CONN_MAX];
static char s_name[BLE_NAME_MAX + 1] = "MiniScheme";

/* the scan's filter: 0 none, 1 address, 2 name prefix, 3 service UUID */
static int s_filter_kind;
static ble_addr_t s_filter_addr;
static char s_filter_name[BLE_NAME_MAX + 1];
static ble_uuid_any_t s_filter_uuid;

/* the Nordic UART service: a phone writes RX, the device notifies TX */
static const ble_uuid128_t NUS_SVC = BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);
static const ble_uuid128_t NUS_RX = BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);
static const ble_uuid128_t NUS_TX = BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);
static uint16_t s_nus_tx_handle;

void ble_store_config_init(void);
static int gap_event(struct ble_gap_event *event, void *arg);

/* ------------------------------------------------------------------------- */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------- */

static void lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }

static void push_event(BleEventType type, uint16_t conn, uint16_t handle, int status, const uint8_t *data, size_t len) {
    if (!s_events) return;
    lock();
    if (s_ev_count == BLE_EVENT_MAX) {      /* full: the oldest leaves */
        s_ev_head = (s_ev_head + 1) % BLE_EVENT_MAX;
        s_ev_count--;
    }
    BleEvent *e = &s_events[(s_ev_head + s_ev_count) % BLE_EVENT_MAX];
    s_ev_count++;
    e->type = (uint8_t)type;
    e->conn = conn;
    e->handle = handle;
    e->status = (int16_t)status;
    e->len = (uint8_t)(len > BLE_EVENT_DATA ? BLE_EVENT_DATA : len);
    if (e->len) memcpy(e->data, data, e->len);
    unlock();
}

static bool take_event(BleEvent *out) {
    bool got = false;
    if (!s_events) return false;
    lock();
    if (s_ev_count > 0) {
        *out = s_events[s_ev_head];
        s_ev_head = (s_ev_head + 1) % BLE_EVENT_MAX;
        s_ev_count--;
        got = true;
    }
    unlock();
    return got;
}

static BleConn *conn_find(uint16_t handle) {
    for (int i = 0; i < BLE_CONN_MAX; i++) {
        if (s_conn[i].used && s_conn[i].handle == handle) return &s_conn[i];
    }
    return NULL;
}

static BleConn *conn_new(uint16_t handle) {
    for (int i = 0; i < BLE_CONN_MAX; i++) {
        if (!s_conn[i].used) {
            memset(&s_conn[i], 0, sizeof(s_conn[i]));
            s_conn[i].used = true;
            s_conn[i].handle = handle;
            s_conn[i].mtu = 23;
            return &s_conn[i];
        }
    }
    return NULL;
}

static void conn_free(BleConn *c) {
    free(c->chr);
    memset(c, 0, sizeof(*c));
}

/* "aa:bb:cc:dd:ee:ff" (most significant byte first) of an address */
static void addr_text(const ble_addr_t *a, char out[18]) {
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", a->val[5], a->val[4], a->val[3], a->val[2], a->val[1], a->val[0]);
}

static bool addr_parse(const char *s, ble_addr_t *a) {
    unsigned v[6];
    if (strlen(s) != 17 || sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x", &v[5], &v[4], &v[3], &v[2], &v[1], &v[0]) != 6) return false;
    for (int i = 0; i < 6; i++) a->val[i] = (uint8_t)v[i];
    a->type = BLE_ADDR_PUBLIC;
    return true;
}

static int hex_digit(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = tolower(c);
    return (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
}

/* Bytes of a hex string ("0a1B", any case; separators - : and spaces skipped); -1 when it is not one */
static int hex_bytes(const char *s, uint8_t *out, size_t cap) {
    size_t n = 0;
    while (*s) {
        if (*s == ' ' || *s == ':' || *s == '-') { s++; continue; }
        int hi = hex_digit((unsigned char)s[0]), lo = s[1] ? hex_digit((unsigned char)s[1]) : -1;
        if (hi < 0 || lo < 0 || n >= cap) return -1;
        out[n++] = (uint8_t)(hi * 16 + lo);
        s += 2;
    }
    return (int)n;
}

static Value hex_string(MiniVM *vm, const uint8_t *data, size_t len) {
    char text[2 * 255 + 1];
    if (len > 255) len = 255;
    for (size_t i = 0; i < len; i++) snprintf(text + 2 * i, 3, "%02x", data[i]);
    text[2 * len] = '\0';
    return mini_make_string(vm, text);
}

/* A UUID: "180f" / "0x180f" (16 bit), or 8-4-4-4-12 hex digits (128 bit) */
static bool uuid_parse(const char *s, ble_uuid_any_t *u) {
    uint8_t b[16];
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    int n = hex_bytes(s, b, sizeof(b));
    if (n == 2) {
        u->u16.u.type = BLE_UUID_TYPE_16;
        u->u16.value = (uint16_t)(b[0] << 8 | b[1]);
        return true;
    }
    if (n == 16) {
        u->u128.u.type = BLE_UUID_TYPE_128;
        for (int i = 0; i < 16; i++) u->u128.value[i] = b[15 - i];   /* NimBLE keeps them least significant first */
        return true;
    }
    return false;
}

static Value uuid_value(MiniVM *vm, const ble_uuid_any_t *u) {
    char text[BLE_UUID_STR_LEN];
    ble_uuid_to_str(&u->u, text);
    return mini_make_string(vm, text[0] == '0' && text[1] == 'x' ? text + 2 : text);
}

static bool running(MiniVM *vm, const char *who) {
    if (s_running && s_synced) return true;
    vm_error(vm, "%s: Bluetooth is off (config.ini [bluetooth] bt_enabled = 1, then restart)", who);
    return false;
}

static void list_push(MiniVM *vm, Value *list, Value v) {
    mini_push_root(vm, &v);
    *list = mini_make_cons(vm, v, *list);
    mini_pop_root(vm);
}

static void alist_put(MiniVM *vm, Value *alist, const char *key, Value v) {
    mini_push_root(vm, &v);
    Value k = mini_make_symbol(vm, key);
    mini_push_root(vm, &k);
    Value pair = mini_make_cons(vm, k, v);
    mini_pop_root(vm);
    mini_pop_root(vm);
    list_push(vm, alist, pair);
}

/* ------------------------------------------------------------------------- */
/* Scanning                                                                  */
/* ------------------------------------------------------------------------- */

static bool scan_matches(const struct ble_gap_disc_desc *d, const struct ble_hs_adv_fields *f) {
    if (s_filter_kind == 0) return true;
    if (s_filter_kind == 1) return memcmp(d->addr.val, s_filter_addr.val, 6) == 0;
    if (s_filter_kind == 2) {
        size_t n = strlen(s_filter_name);
        return f->name && f->name_len >= n && memcmp(f->name, s_filter_name, n) == 0;
    }
    for (int i = 0; i < f->num_uuids16; i++) {
        if (ble_uuid_cmp(&f->uuids16[i].u, &s_filter_uuid.u) == 0) return true;
    }
    for (int i = 0; i < f->num_uuids128; i++) {
        if (ble_uuid_cmp(&f->uuids128[i].u, &s_filter_uuid.u) == 0) return true;
    }
    if (f->svc_data_uuid16 && f->svc_data_uuid16_len >= 2 && s_filter_uuid.u.type == BLE_UUID_TYPE_16 &&
        (uint16_t)(f->svc_data_uuid16[0] | f->svc_data_uuid16[1] << 8) == s_filter_uuid.u16.value) return true;
    return false;
}

/* A report into the table: its device's row, a free one, or the weakest row when stronger */
static void scan_report(const struct ble_gap_disc_desc *d) {
    struct ble_hs_adv_fields f;
    bool rsp = d->event_type == BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP;
    if (ble_hs_adv_parse_fields(&f, d->data, d->length_data) != 0) memset(&f, 0, sizeof(f));
    lock();
    BleSeen *row = NULL, *weakest = NULL;
    for (int i = 0; i < s_seen_count; i++) {
        if (ble_addr_cmp(&s_seen[i].addr, &d->addr) == 0) { row = &s_seen[i]; break; }
        if (!weakest || s_seen[i].rssi < weakest->rssi) weakest = &s_seen[i];
    }
    if (!row) {
        /* a scan response alone (its advertisement was filtered out or missed) starts no row */
        if (rsp || !scan_matches(d, &f)) { unlock(); return; }
        if (s_seen_count < BLE_SCAN_MAX) row = &s_seen[s_seen_count++];
        else if (weakest && d->rssi > weakest->rssi) row = weakest;
        else { unlock(); return; }
        memset(row, 0, sizeof(*row));
        row->addr = d->addr;
    }
    row->rssi = d->rssi;
    row->seen_us = esp_timer_get_time();
    uint8_t n = d->length_data > 31 ? 31 : d->length_data;
    if (rsp) {
        memcpy(row->rsp, d->data, n);
        row->rsp_len = n;
    } else {
        memcpy(row->adv, d->data, n);
        row->adv_len = n;
        row->connectable = d->event_type == BLE_HCI_ADV_RPT_EVTYPE_ADV_IND || d->event_type == BLE_HCI_ADV_RPT_EVTYPE_DIR_IND;
    }
    if (f.name && f.name_len) {
        size_t k = f.name_len > BLE_NAME_MAX ? BLE_NAME_MAX : f.name_len;
        memcpy(row->name, f.name, k);
        row->name[k] = '\0';
    }
    unlock();
}

/* (ble-scan-start [seconds [active? [filter]]]) */
static Value prim_scan_start(MiniVM *vm, int argc, Value *argv) {
    if (!running(vm, "ble-scan-start")) return mini_void();
    int32_t ms = BLE_HS_FOREVER;
    bool active = true;
    if (argc >= 1 && mini_is_fixnum(argv[0]) && mini_to_fixnum(argv[0]) > 0) ms = (int32_t)mini_to_fixnum(argv[0]) * 1000;
    if (argc >= 2) active = !mini_is_false(argv[1]);
    s_filter_kind = 0;
    if (argc >= 3 && mini_is_string(argv[2]) && mini_string_data(argv[2])[0]) {
        const char *f = mini_string_data(argv[2]);
        if (addr_parse(f, &s_filter_addr)) s_filter_kind = 1;
        else if (strncmp(f, "name:", 5) == 0) { snprintf(s_filter_name, sizeof(s_filter_name), "%s", f + 5); s_filter_kind = 2; }
        else if (uuid_parse(f, &s_filter_uuid)) s_filter_kind = 3;
        else {
            vm_error(vm, "ble-scan-start: the filter is an address, \"name:<prefix>\" or a service UUID");
            return mini_void();
        }
    }
    if (s_scanning) ble_gap_disc_cancel();
    lock();
    s_seen_count = 0;
    unlock();
    struct ble_gap_disc_params p = { .passive = !active, .itvl = 0x0050, .window = 0x0030, .filter_duplicates = 0 };
    int rc = ble_gap_disc(s_own_addr_type, ms, &p, gap_event, NULL);
    s_scanning = rc == 0;
    if (rc != 0) ESP_LOGW(TAG, "scan: rc=%d", rc);
    return mini_make_bool(rc == 0);
}

static Value prim_scan_stop(MiniVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc; (void)argv;
    if (s_running && s_scanning) ble_gap_disc_cancel();
    s_scanning = false;
    return mini_true();
}

/* (ble-scan-results): ((address type rssi name connectable? adv-hex) ...), the strongest first */
static Value prim_scan_results(MiniVM *vm, int argc, Value *argv) {
    (void)argc; (void)argv;
    Value out = mini_nil();
    if (!s_seen) return out;
    BleSeen *rows = (BleSeen *)malloc(sizeof(BleSeen) * BLE_SCAN_MAX);
    if (!rows) return out;
    lock();
    int n = s_seen_count;
    memcpy(rows, s_seen, sizeof(BleSeen) * (size_t)n);
    unlock();
    for (int i = 1; i < n; i++) {           /* the weakest first: the list is built from its end */
        BleSeen key = rows[i];
        int j = i - 1;
        while (j >= 0 && rows[j].rssi > key.rssi) { rows[j + 1] = rows[j]; j--; }
        rows[j + 1] = key;
    }
    mini_push_root(vm, &out);
    for (int i = 0; i < n; i++) {
        BleSeen *r = &rows[i];
        char addr[18];
        uint8_t both[62];
        addr_text(&r->addr, addr);
        memcpy(both, r->adv, r->adv_len);
        memcpy(both + r->adv_len, r->rsp, r->rsp_len);
        Value row = mini_nil();
        mini_push_root(vm, &row);
        list_push(vm, &row, hex_string(vm, both, (size_t)r->adv_len + r->rsp_len));
        list_push(vm, &row, mini_make_bool(r->connectable));
        list_push(vm, &row, mini_make_string(vm, r->name));
        list_push(vm, &row, mini_make_fixnum(r->rssi));
        list_push(vm, &row, mini_make_symbol(vm, r->addr.type == BLE_ADDR_PUBLIC ? "public" : "random"));
        list_push(vm, &row, mini_make_string(vm, addr));
        out = mini_make_cons(vm, row, out);
        mini_pop_root(vm);
    }
    mini_pop_root(vm);
    free(rows);
    return out;
}

/* (ble-adv-field adv-hex type): the bytes (hex) of the first AD structure of that type, or #f */
static Value prim_adv_field(MiniVM *vm, int argc, Value *argv) {
    uint8_t b[64];
    if (argc != 2 || !mini_is_string(argv[0]) || !mini_is_fixnum(argv[1])) {
        vm_error(vm, "ble-adv-field: expected the advertisement (hex) and an AD type (number)");
        return mini_void();
    }
    int n = hex_bytes(mini_string_data(argv[0]), b, sizeof(b));
    int want = (int)mini_to_fixnum(argv[1]);
    for (int i = 0; n > 0 && i + 1 < n; ) {
        int len = b[i];
        if (len == 0 || i + 1 + len > n) break;
        if (b[i + 1] == want) return hex_string(vm, b + i + 2, (size_t)len - 1);
        i += 1 + len;
    }
    return mini_false();
}

/* ------------------------------------------------------------------------- */
/* Connections: the central side                                             */
/* ------------------------------------------------------------------------- */

static int on_chr(uint16_t conn_handle, const struct ble_gatt_error *error, const struct ble_gatt_chr *chr, void *arg) {
    (void)arg;
    lock();
    BleConn *c = conn_find(conn_handle);
    if (c && error->status == 0 && c->chr && c->nchr < BLE_CHR_MAX) {
        BleChr *k = &c->chr[c->nchr++];
        k->def_handle = chr->def_handle;
        k->val_handle = chr->val_handle;
        k->props = chr->properties;
        k->uuid = chr->uuid;
    }
    bool done = error->status != 0;
    if (c && done) c->discover = error->status == BLE_HS_EDONE ? 3 : 4;
    unlock();
    if (done) push_event(EV_DISCOVERED, conn_handle, 0, error->status == BLE_HS_EDONE ? 0 : error->status, NULL, 0);
    return 0;
}

static int on_svc(uint16_t conn_handle, const struct ble_gatt_error *error, const struct ble_gatt_svc *svc, void *arg) {
    (void)arg;
    lock();
    BleConn *c = conn_find(conn_handle);
    if (c && error->status == 0 && c->nsvc < BLE_SVC_MAX) {
        BleSvc *s = &c->svc[c->nsvc++];
        s->start = svc->start_handle;
        s->end = svc->end_handle;
        s->uuid = svc->uuid;
    }
    if (c && error->status == BLE_HS_EDONE) c->discover = 2;
    else if (c && error->status != 0) c->discover = 4;
    unlock();
    if (error->status == BLE_HS_EDONE) {
        if (ble_gattc_disc_all_chrs(conn_handle, 1, 0xffff, on_chr, NULL) != 0) {
            push_event(EV_DISCOVERED, conn_handle, 0, BLE_HS_EUNKNOWN, NULL, 0);
        }
    } else if (error->status != 0) {
        push_event(EV_DISCOVERED, conn_handle, 0, error->status, NULL, 0);
    }
    return 0;
}

static int on_read(uint16_t conn_handle, const struct ble_gatt_error *error, struct ble_gatt_attr *attr, void *arg) {
    uint8_t data[BLE_EVENT_DATA];
    uint16_t len = 0;
    uint16_t handle = (uint16_t)(uintptr_t)arg;
    if (error->status == 0 && attr && attr->om) ble_hs_mbuf_to_flat(attr->om, data, sizeof(data), &len);
    push_event(EV_READ, conn_handle, handle, error->status, data, len);
    return 0;
}

static int on_write(uint16_t conn_handle, const struct ble_gatt_error *error, struct ble_gatt_attr *attr, void *arg) {
    (void)attr;
    push_event(EV_WRITTEN, conn_handle, (uint16_t)(uintptr_t)arg, error->status, NULL, 0);
    return 0;
}

static int on_cccd_write(uint16_t conn_handle, const struct ble_gatt_error *error, struct ble_gatt_attr *attr, void *arg) {
    (void)attr;
    push_event(EV_SUBSCRIBED, conn_handle, (uint16_t)(uintptr_t)arg, error->status, NULL, 0);
    return 0;
}

/* The descriptors of the subscribed characteristic: its CCCD (0x2902) gets the request */
static int on_dsc(uint16_t conn_handle, const struct ble_gatt_error *error, uint16_t chr_val_handle,
                  const struct ble_gatt_dsc *dsc, void *arg) {
    (void)arg;
    BleConn *c = conn_find(conn_handle);
    if (!c) return 0;
    if (error->status == 0 && !c->sub_found && chr_val_handle == c->sub_handle &&
        ble_uuid_u16(&dsc->uuid.u) == BLE_GATT_DSC_CLT_CFG_UUID16) {
        uint8_t v[2] = { c->sub_on ? 1 : 0, 0 };
        c->sub_found = true;
        if (ble_gattc_write_flat(conn_handle, dsc->handle, v, 2, on_cccd_write, (void *)(uintptr_t)c->sub_handle) != 0) {
            push_event(EV_SUBSCRIBED, conn_handle, c->sub_handle, BLE_HS_EUNKNOWN, NULL, 0);
        }
    } else if (error->status != 0 && !c->sub_found) {
        push_event(EV_SUBSCRIBED, conn_handle, c->sub_handle, error->status == BLE_HS_EDONE ? BLE_HS_ENOENT : error->status, NULL, 0);
    }
    return 0;
}

static bool conn_arg(MiniVM *vm, const char *who, int argc, Value *argv, BleConn **out) {
    if (!running(vm, who)) return false;
    if (argc < 1 || !mini_is_fixnum(argv[0])) {
        vm_error(vm, "%s: expected a connection (the number of its connected event)", who);
        return false;
    }
    *out = conn_find((uint16_t)mini_to_fixnum(argv[0]));
    return true;
}

/* (ble-connect address [type]): the outcome is a connected or a disconnected event */
static Value prim_connect(MiniVM *vm, int argc, Value *argv) {
    ble_addr_t peer;
    if (!running(vm, "ble-connect")) return mini_void();
    if (argc < 1 || !mini_is_string(argv[0]) || !addr_parse(mini_string_data(argv[0]), &peer)) {
        vm_error(vm, "ble-connect: expected an address \"aa:bb:cc:dd:ee:ff\"");
        return mini_void();
    }
    bool typed = false;
    if (argc >= 2 && mini_is_symbol(argv[1])) {
        peer.type = strcmp(mini_symbol_name(argv[1]), "random") == 0 ? BLE_ADDR_RANDOM : BLE_ADDR_PUBLIC;
        typed = true;
    }
    lock();
    for (int i = 0; !typed && i < s_seen_count; i++) {     /* the type the scan saw */
        if (memcmp(s_seen[i].addr.val, peer.val, 6) == 0) peer.type = s_seen[i].addr.type;
    }
    unlock();
    if (s_connecting) return mini_false();
    if (s_scanning) { ble_gap_disc_cancel(); s_scanning = false; }
    int rc = ble_gap_connect(s_own_addr_type, &peer, 10000, NULL, gap_event, NULL);
    s_connecting = rc == 0;
    if (rc != 0) ESP_LOGW(TAG, "connect: rc=%d", rc);
    return mini_make_bool(rc == 0);
}

static Value prim_disconnect(MiniVM *vm, int argc, Value *argv) {
    BleConn *c = NULL;
    if (!conn_arg(vm, "ble-disconnect", argc, argv, &c)) return mini_void();
    if (!c) {
        if (s_connecting) { ble_gap_conn_cancel(); s_connecting = false; }
        return mini_false();
    }
    return mini_make_bool(ble_gap_terminate(c->handle, BLE_ERR_REM_USER_CONN_TERM) == 0);
}

/* (ble-connections): ((conn address role mtu encrypted?) ...) */
static Value prim_connections(MiniVM *vm, int argc, Value *argv) {
    (void)argc; (void)argv;
    Value out = mini_nil();
    mini_push_root(vm, &out);
    for (int i = BLE_CONN_MAX - 1; i >= 0; i--) {
        BleConn c;
        lock();
        c = s_conn[i];
        unlock();
        if (!c.used) continue;
        char addr[18];
        addr_text(&c.peer, addr);
        Value row = mini_nil();
        mini_push_root(vm, &row);
        list_push(vm, &row, mini_make_bool(c.encrypted));
        list_push(vm, &row, mini_make_fixnum(c.mtu));
        list_push(vm, &row, mini_make_symbol(vm, c.central ? "central" : "peripheral"));
        list_push(vm, &row, mini_make_string(vm, addr));
        list_push(vm, &row, mini_make_fixnum(c.handle));
        out = mini_make_cons(vm, row, out);
        mini_pop_root(vm);
    }
    mini_pop_root(vm);
    return out;
}

/* (ble-discover conn): a discovered event, then (ble-services conn) */
static Value prim_discover(MiniVM *vm, int argc, Value *argv) {
    BleConn *c = NULL;
    if (!conn_arg(vm, "ble-discover", argc, argv, &c)) return mini_void();
    if (!c) return mini_false();
    lock();
    if (!c->chr) c->chr = (BleChr *)calloc(BLE_CHR_MAX, sizeof(BleChr));
    c->nsvc = c->nchr = 0;
    c->discover = c->chr ? 1 : 4;
    unlock();
    if (!c->chr) return mini_false();
    return mini_make_bool(ble_gattc_disc_all_svcs(c->handle, on_svc, NULL) == 0);
}

static Value props_list(MiniVM *vm, uint8_t props) {
    static const struct { uint8_t bit; const char *name; } P[] = {
        { BLE_GATT_CHR_PROP_INDICATE, "indicate" }, { BLE_GATT_CHR_PROP_NOTIFY, "notify" },
        { BLE_GATT_CHR_PROP_WRITE_NO_RSP, "write-no-response" }, { BLE_GATT_CHR_PROP_WRITE, "write" },
        { BLE_GATT_CHR_PROP_READ, "read" },
    };
    Value l = mini_nil();
    mini_push_root(vm, &l);
    for (size_t i = 0; i < sizeof(P) / sizeof(P[0]); i++) {
        if (props & P[i].bit) list_push(vm, &l, mini_make_symbol(vm, P[i].name));
    }
    mini_pop_root(vm);
    return l;
}

/* (ble-services conn): ((service-uuid (characteristic-uuid handle (read write notify ...)) ...) ...),
 * #f until a discovery has finished */
static Value prim_services(MiniVM *vm, int argc, Value *argv) {
    BleConn *c = NULL;
    if (!conn_arg(vm, "ble-services", argc, argv, &c)) return mini_void();
    if (!c || c->discover != 3) return mini_false();
    Value out = mini_nil();
    mini_push_root(vm, &out);
    for (int s = c->nsvc - 1; s >= 0; s--) {
        Value svc = mini_nil();
        mini_push_root(vm, &svc);
        for (int k = c->nchr - 1; k >= 0; k--) {
            const BleChr *ch = &c->chr[k];
            if (ch->def_handle < c->svc[s].start || ch->def_handle > c->svc[s].end) continue;
            Value row = mini_nil();
            mini_push_root(vm, &row);
            list_push(vm, &row, props_list(vm, ch->props));
            list_push(vm, &row, mini_make_fixnum(ch->val_handle));
            list_push(vm, &row, uuid_value(vm, &ch->uuid));
            svc = mini_make_cons(vm, row, svc);
            mini_pop_root(vm);
        }
        list_push(vm, &svc, uuid_value(vm, &c->svc[s].uuid));
        out = mini_make_cons(vm, svc, out);
        mini_pop_root(vm);
    }
    mini_pop_root(vm);
    return out;
}

/* A value to write: a hex string, or a list of byte numbers */
static int value_bytes(Value v, uint8_t *out, size_t cap) {
    if (mini_is_string(v)) return hex_bytes(mini_string_data(v), out, cap);
    size_t n = 0;
    for (; mini_is_pair(v); v = mini_cdr(v)) {
        Value b = mini_car(v);
        if (!mini_is_fixnum(b) || mini_to_fixnum(b) < 0 || mini_to_fixnum(b) > 255 || n >= cap) return -1;
        out[n++] = (uint8_t)mini_to_fixnum(b);
    }
    return (int)n;
}

static bool handle_arg(MiniVM *vm, const char *who, int argc, Value *argv, BleConn **c, uint16_t *handle) {
    if (!conn_arg(vm, who, argc, argv, c)) return false;
    if (argc < 2 || !mini_is_fixnum(argv[1])) {
        vm_error(vm, "%s: expected a connection and a characteristic's handle", who);
        return false;
    }
    *handle = (uint16_t)mini_to_fixnum(argv[1]);
    return true;
}

/* (ble-read conn handle): a read event with the value */
static Value prim_read(MiniVM *vm, int argc, Value *argv) {
    BleConn *c = NULL;
    uint16_t h = 0;
    if (!handle_arg(vm, "ble-read", argc, argv, &c, &h)) return mini_void();
    if (!c) return mini_false();
    return mini_make_bool(ble_gattc_read(c->handle, h, on_read, (void *)(uintptr_t)h) == 0);
}

/* (ble-write conn handle value [no-response?]): a written event unless without response */
static Value prim_write(MiniVM *vm, int argc, Value *argv) {
    BleConn *c = NULL;
    uint16_t h = 0;
    uint8_t data[BLE_EVENT_DATA];
    if (!handle_arg(vm, "ble-write", argc, argv, &c, &h)) return mini_void();
    int n = argc >= 3 ? value_bytes(argv[2], data, sizeof(data)) : -1;
    if (n < 0) {
        vm_error(vm, "ble-write: the value is a hex string or a list of bytes (up to %d)", BLE_EVENT_DATA);
        return mini_void();
    }
    if (!c) return mini_false();
    if (argc >= 4 && !mini_is_false(argv[3])) return mini_make_bool(ble_gattc_write_no_rsp_flat(c->handle, h, data, (uint16_t)n) == 0);
    return mini_make_bool(ble_gattc_write_flat(c->handle, h, data, (uint16_t)n, on_write, (void *)(uintptr_t)h) == 0);
}

/* (ble-subscribe conn handle [on?]): a subscribed event; then notify events with the values */
static Value prim_subscribe(MiniVM *vm, int argc, Value *argv) {
    BleConn *c = NULL;
    uint16_t h = 0;
    if (!handle_arg(vm, "ble-subscribe", argc, argv, &c, &h)) return mini_void();
    if (!c) return mini_false();
    uint16_t end = 0xffff;
    lock();
    for (int k = 0; k < c->nchr; k++) {          /* up to the next characteristic, or its service's end */
        if (c->chr[k].val_handle != h) continue;
        for (int s = 0; s < c->nsvc; s++) {
            if (h >= c->svc[s].start && h <= c->svc[s].end) end = c->svc[s].end;
        }
        if (k + 1 < c->nchr && c->chr[k + 1].def_handle > h && c->chr[k + 1].def_handle - 1 < end) end = c->chr[k + 1].def_handle - 1;
    }
    c->sub_handle = h;
    c->sub_on = argc < 3 || !mini_is_false(argv[2]);
    c->sub_found = false;
    unlock();
    if (end <= h) return mini_false();
    return mini_make_bool(ble_gattc_disc_all_dscs(c->handle, h, end, on_dsc, NULL) == 0);
}

/* (ble-pair conn): encrypt the link (pairing and bonding when needed); a paired event */
static Value prim_pair(MiniVM *vm, int argc, Value *argv) {
    BleConn *c = NULL;
    if (!conn_arg(vm, "ble-pair", argc, argv, &c)) return mini_void();
    if (!c) return mini_false();
#if BLE_SECURITY
    return mini_make_bool(ble_gap_security_initiate(c->handle) == 0);
#else
    return mini_false();   /* built without the security manager */
#endif
}

/* (ble-bonds): the addresses of the bonded devices */
static Value prim_bonds(MiniVM *vm, int argc, Value *argv) {
    (void)argc; (void)argv;
    Value out = mini_nil();
#if BLE_SECURITY
    if (!s_running) return out;
    ble_addr_t peers[CONFIG_BT_NIMBLE_MAX_BONDS];
    int n = 0;
    if (ble_store_util_bonded_peers(peers, &n, CONFIG_BT_NIMBLE_MAX_BONDS) != 0) return out;
    mini_push_root(vm, &out);
    for (int i = n - 1; i >= 0; i--) {
        char addr[18];
        addr_text(&peers[i], addr);
        list_push(vm, &out, mini_make_string(vm, addr));
    }
    mini_pop_root(vm);
#else
    (void)vm;
#endif
    return out;
}

/* (ble-unpair address) / (ble-unpair #t): forget one bond, or all */
static Value prim_unpair(MiniVM *vm, int argc, Value *argv) {
    if (!running(vm, "ble-unpair")) return mini_void();
#if !BLE_SECURITY
    (void)argc; (void)argv;
    return mini_false();
#else
    if (argc == 1 && mini_is_string(argv[0])) {
        ble_addr_t a, peers[CONFIG_BT_NIMBLE_MAX_BONDS];
        int n = 0;
        if (!addr_parse(mini_string_data(argv[0]), &a)) return mini_false();
        ble_store_util_bonded_peers(peers, &n, CONFIG_BT_NIMBLE_MAX_BONDS);
        for (int i = 0; i < n; i++) {
            if (memcmp(peers[i].val, a.val, 6) == 0) return mini_make_bool(ble_gap_unpair(&peers[i]) == 0);
        }
        return mini_false();
    }
    return mini_make_bool(ble_store_clear() == 0);
#endif
}

/* ------------------------------------------------------------------------- */
/* The peripheral side: advertising, the UART service                        */
/* ------------------------------------------------------------------------- */

static int nus_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)attr_handle; (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint8_t data[BLE_EVENT_DATA];
        uint16_t len = 0;
        ble_hs_mbuf_to_flat(ctxt->om, data, sizeof(data), &len);
        push_event(EV_UART, conn_handle, 0, 0, data, len);
    }
    return 0;
}

static const struct ble_gatt_svc_def GATT_SERVICES[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &NUS_SVC.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = &NUS_RX.u, .access_cb = nus_access, .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP },
            { .uuid = &NUS_TX.u, .access_cb = nus_access, .val_handle = &s_nus_tx_handle, .flags = BLE_GATT_CHR_F_NOTIFY },
            { 0 },
        },
    },
    { 0 },
};

/* (ble-advertise [connectable? [manufacturer-data]]): the device's name and, connectable,
 * the UART service; manufacturer-data: hex, company id first */
static Value prim_advertise(MiniVM *vm, int argc, Value *argv) {
    if (!running(vm, "ble-advertise")) return mini_void();
    bool connectable = argc < 1 || !mini_is_false(argv[0]);
    uint8_t mfg[24];
    int mfg_len = 0;
    if (argc >= 2 && !mini_is_false(argv[1])) {
        mfg_len = value_bytes(argv[1], mfg, sizeof(mfg));
        if (mfg_len < 2) {
            vm_error(vm, "ble-advertise: manufacturer data is hex or bytes, the company id (2 bytes) first, up to 24");
            return mini_void();
        }
    }
    if (s_advertising) ble_gap_adv_stop();
    struct ble_hs_adv_fields f, r;
    memset(&f, 0, sizeof(f));
    memset(&r, 0, sizeof(r));
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    if (mfg_len) {
        f.mfg_data = mfg;
        f.mfg_data_len = (uint8_t)mfg_len;
    }
    /* the name beside the flags and the data when it fits, else in the scan response */
    size_t name_len = strlen(s_name);
    bool name_in_adv = 3 + (mfg_len ? 2 + (size_t)mfg_len : 0) + 2 + name_len <= 31;
    struct ble_hs_adv_fields *nf = name_in_adv ? &f : &r;
    nf->name = (const uint8_t *)s_name;
    nf->name_len = (uint8_t)name_len;
    nf->name_is_complete = 1;
    if (connectable && (!name_in_adv || name_len + 2 + 18 <= 31)) {
        r.uuids128 = &NUS_SVC;
        r.num_uuids128 = 1;
        r.uuids128_is_complete = 1;
    }
    int rc = ble_gap_adv_set_fields(&f);
    if (rc == 0) rc = ble_gap_adv_rsp_set_fields(&r);
    struct ble_gap_adv_params p;
    memset(&p, 0, sizeof(p));
    p.conn_mode = connectable ? BLE_GAP_CONN_MODE_UND : BLE_GAP_CONN_MODE_NON;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    if (rc == 0) rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
    s_advertising = rc == 0;
    if (rc != 0) ESP_LOGW(TAG, "advertise: rc=%d", rc);
    return mini_make_bool(rc == 0);
}

static Value prim_advertise_stop(MiniVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc; (void)argv;
    if (s_running && s_advertising) ble_gap_adv_stop();
    s_advertising = false;
    return mini_true();
}

/* (ble-uart-write text [conn]): to every subscribed central, or one; the number reached */
static Value prim_uart_write(MiniVM *vm, int argc, Value *argv) {
    if (!running(vm, "ble-uart-write")) return mini_void();
    if (argc < 1 || !mini_is_string(argv[0])) {
        vm_error(vm, "ble-uart-write: expected a string");
        return mini_void();
    }
    const char *text = mini_string_data(argv[0]);
    size_t len = strlen(text);
    int reached = 0;
    for (int i = 0; i < BLE_CONN_MAX; i++) {
        if (!s_conn[i].used || s_conn[i].central) continue;
        if (argc >= 2 && mini_is_fixnum(argv[1]) && mini_to_fixnum(argv[1]) != s_conn[i].handle) continue;
        size_t chunk = s_conn[i].mtu > 3 ? (size_t)s_conn[i].mtu - 3 : 20, sent = 0;
        bool ok = true;
        while (ok && sent < len) {
            size_t n = len - sent < chunk ? len - sent : chunk;
            struct os_mbuf *om = ble_hs_mbuf_from_flat(text + sent, (uint16_t)n);
            ok = om && ble_gatts_notify_custom(s_conn[i].handle, s_nus_tx_handle, om) == 0;
            sent += n;
        }
        if (ok) reached++;
    }
    return mini_make_fixnum(reached);
}

/* ------------------------------------------------------------------------- */
/* Events                                                                    */
/* ------------------------------------------------------------------------- */

static int gap_event(struct ble_gap_event *event, void *arg) {
    (void)arg;
    struct ble_gap_conn_desc desc;
    switch (event->type) {
        case BLE_GAP_EVENT_DISC:
            scan_report(&event->disc);
            break;
        case BLE_GAP_EVENT_DISC_COMPLETE:
            s_scanning = false;
            push_event(EV_SCAN_DONE, 0, 0, s_seen_count, NULL, 0);
            break;
        case BLE_GAP_EVENT_CONNECT: {
            bool ours = ble_gap_conn_find(event->connect.conn_handle, &desc) == 0 && desc.role == BLE_GAP_ROLE_MASTER;
            if (event->connect.status != 0) {
                if (s_connecting) {
                    s_connecting = false;
                    push_event(EV_DISCONNECTED, 0xffff, 0, event->connect.status, NULL, 0);
                } else {
                    s_advertising = false;
                }
                break;
            }
            if (ours) s_connecting = false;
            else s_advertising = false;         /* connectable advertising ends with a connection */
            lock();
            BleConn *c = conn_new(event->connect.conn_handle);
            if (c) {
                c->central = ours;
                c->peer = desc.peer_id_addr;
            }
            unlock();
            char addr[18];
            addr_text(&desc.peer_id_addr, addr);
            push_event(EV_CONNECTED, event->connect.conn_handle, 0, ours ? 1 : 0, (const uint8_t *)addr, 17);
            if (ours) ble_gattc_exchange_mtu(event->connect.conn_handle, NULL, NULL);
            break;
        }
        case BLE_GAP_EVENT_DISCONNECT:
            lock();
            {
                BleConn *c = conn_find(event->disconnect.conn.conn_handle);
                if (c) conn_free(c);
            }
            unlock();
            push_event(EV_DISCONNECTED, event->disconnect.conn.conn_handle, 0, event->disconnect.reason, NULL, 0);
            break;
        case BLE_GAP_EVENT_ADV_COMPLETE:
            s_advertising = false;
            break;
        case BLE_GAP_EVENT_MTU: {
            BleConn *c = conn_find(event->mtu.conn_handle);
            if (c) c->mtu = event->mtu.value;
            break;
        }
        case BLE_GAP_EVENT_ENC_CHANGE: {
            BleConn *c = conn_find(event->enc_change.conn_handle);
            if (c) c->encrypted = event->enc_change.status == 0;
            push_event(EV_PAIRED, event->enc_change.conn_handle, 0, event->enc_change.status, NULL, 0);
            break;
        }
#if BLE_SECURITY
        case BLE_GAP_EVENT_REPEAT_PAIRING:
            /* the peer lost its bond: ours goes, and the pairing starts over */
            if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) ble_store_util_delete_peer(&desc.peer_id_addr);
            return BLE_GAP_REPEAT_PAIRING_RETRY;
#endif
        case BLE_GAP_EVENT_NOTIFY_RX: {
            uint8_t data[BLE_EVENT_DATA];
            uint16_t len = 0;
            ble_hs_mbuf_to_flat(event->notify_rx.om, data, sizeof(data), &len);
            push_event(EV_NOTIFY, event->notify_rx.conn_handle, event->notify_rx.attr_handle, 0, data, len);
            break;
        }
        default:
            break;
    }
    return 0;
}

static Value event_value(MiniVM *vm, const BleEvent *e) {
    static const char *const NAMES[] = { "", "connected", "disconnected", "discovered", "read", "written",
                                         "subscribed", "notify", "uart", "scan-done", "paired" };
    Value l = mini_nil();
    mini_push_root(vm, &l);
    switch (e->type) {
        case EV_CONNECTED:      /* (connected conn address role) */
            list_push(vm, &l, mini_make_symbol(vm, e->status ? "central" : "peripheral"));
            list_push(vm, &l, mini_make_string_len(vm, (const char *)e->data, e->len));
            list_push(vm, &l, mini_make_fixnum(e->conn));
            break;
        case EV_DISCONNECTED:   /* (disconnected conn reason); conn #f: a connection that never came up */
            list_push(vm, &l, mini_make_fixnum(e->status));
            list_push(vm, &l, e->conn == 0xffff ? mini_false() : mini_make_fixnum(e->conn));
            break;
        case EV_DISCOVERED:     /* (discovered conn ok?) */
        case EV_PAIRED:         /* (paired conn ok?) */
            list_push(vm, &l, mini_make_bool(e->status == 0));
            list_push(vm, &l, mini_make_fixnum(e->conn));
            break;
        case EV_READ:           /* (read conn handle value-or-#f) */
            list_push(vm, &l, e->status == 0 ? hex_string(vm, e->data, e->len) : mini_false());
            list_push(vm, &l, mini_make_fixnum(e->handle));
            list_push(vm, &l, mini_make_fixnum(e->conn));
            break;
        case EV_WRITTEN:        /* (written conn handle ok?) */
        case EV_SUBSCRIBED:     /* (subscribed conn handle ok?) */
            list_push(vm, &l, mini_make_bool(e->status == 0));
            list_push(vm, &l, mini_make_fixnum(e->handle));
            list_push(vm, &l, mini_make_fixnum(e->conn));
            break;
        case EV_NOTIFY:         /* (notify conn handle value) */
            list_push(vm, &l, hex_string(vm, e->data, e->len));
            list_push(vm, &l, mini_make_fixnum(e->handle));
            list_push(vm, &l, mini_make_fixnum(e->conn));
            break;
        case EV_UART: {         /* (uart conn text): up to the first zero byte */
            size_t n = 0;
            while (n < e->len && e->data[n]) n++;
            list_push(vm, &l, mini_make_string_len(vm, (const char *)e->data, n));
            list_push(vm, &l, mini_make_fixnum(e->conn));
            break;
        }
        case EV_SCAN_DONE:      /* (scan-done devices) */
            list_push(vm, &l, mini_make_fixnum(e->status));
            break;
        default:
            break;
    }
    list_push(vm, &l, mini_make_symbol(vm, NAMES[e->type <= EV_PAIRED ? e->type : 0]));
    mini_pop_root(vm);
    return l;
}

/* (ble-event): the next event, or #f */
static Value prim_event(MiniVM *vm, int argc, Value *argv) {
    (void)argc; (void)argv;
    BleEvent e;
    return take_event(&e) ? event_value(vm, &e) : mini_false();
}

void ble_poll(MiniVM *vm) {
    BleEvent e;
    if (!s_running || !vm || s_ev_count == 0 || !mini_device_has_hook(vm, "app-ble-event")) return;
    for (int i = 0; i < 4 && take_event(&e); i++) {
        Value ev = event_value(vm, &e);
        mini_push_root(vm, &ev);
        mini_device_call_hook(vm, "app-ble-event", NULL, 1, &ev);
        mini_pop_root(vm);
    }
}

/* ------------------------------------------------------------------------- */
/* Status, conversions                                                       */
/* ------------------------------------------------------------------------- */

static const char *state_name(void) {
    if (!s_running) return "off";
    if (!s_synced) return "starting";
    if (s_connecting) return "connecting";
    if (s_scanning) return "scanning";
    if (s_advertising) return "advertising";
    return "idle";
}

static void own_address(char out[18]) {
    ble_addr_t a = { 0 };
    out[0] = '\0';
    if (s_synced && ble_hs_id_copy_addr(s_own_addr_type, a.val, NULL) == 0) addr_text(&a, out);
}

static int connection_count(void) {
    int n = 0;
    for (int i = 0; i < BLE_CONN_MAX; i++) n += s_conn[i].used;
    return n;
}

/* (ble-status): ((enabled . #t) (state . idle) (address . "..") (name . "..") (connections . 1) (heap . 30712)) */
static Value prim_status(MiniVM *vm, int argc, Value *argv) {
    (void)argc; (void)argv;
    char addr[18];
    Value l = mini_nil();
    own_address(addr);
    mini_push_root(vm, &l);
    alist_put(vm, &l, "heap", mini_make_fixnum((int64_t)s_heap_used));
    alist_put(vm, &l, "connections", mini_make_fixnum(connection_count()));
    alist_put(vm, &l, "name", mini_make_string(vm, s_name));
    alist_put(vm, &l, "address", mini_make_string(vm, addr));
    alist_put(vm, &l, "state", mini_make_symbol(vm, state_name()));
    alist_put(vm, &l, "enabled", mini_make_bool(s_running));
    mini_pop_root(vm);
    return l;
}

size_t ble_status_json(char *out, size_t cap) {
    char addr[18];
    own_address(addr);
    int n = snprintf(out, cap, "{\"enabled\":%s,\"state\":\"%s\",\"address\":\"%s\",\"name\":\"%s\",\"connections\":%d,"
                               "\"seen\":%d,\"heap\":%u}",
                     s_running ? "true" : "false", state_name(), addr, s_name, connection_count(), s_seen_count,
                     (unsigned)s_heap_used);
    return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}

/* (ble-bytes->hex list) and (ble-hex->bytes hex) */
static Value prim_bytes_hex(MiniVM *vm, int argc, Value *argv) {
    uint8_t b[255];
    int n = argc == 1 ? value_bytes(argv[0], b, sizeof(b)) : -1;
    if (n < 0 || (argc == 1 && mini_is_string(argv[0]))) {
        vm_error(vm, "ble-bytes->hex: expected a list of bytes (0..255)");
        return mini_void();
    }
    return hex_string(vm, b, (size_t)n);
}

static Value prim_hex_bytes(MiniVM *vm, int argc, Value *argv) {
    uint8_t b[255];
    int n = argc == 1 && mini_is_string(argv[0]) ? hex_bytes(mini_string_data(argv[0]), b, sizeof(b)) : -1;
    if (n < 0) {
        vm_error(vm, "ble-hex->bytes: expected a hex string");
        return mini_void();
    }
    Value l = mini_nil();
    mini_push_root(vm, &l);
    for (int i = n - 1; i >= 0; i--) list_push(vm, &l, mini_make_fixnum(b[i]));
    mini_pop_root(vm);
    return l;
}

void ble_register(MiniVM *vm) {
    static const MiniPrimEntry PRIMS[] = {
        { "ble-status", prim_status }, { "ble-event", prim_event },
        { "ble-scan-start", prim_scan_start }, { "ble-scan-stop", prim_scan_stop },
        { "ble-scan-results", prim_scan_results }, { "ble-adv-field", prim_adv_field },
        { "ble-connect", prim_connect }, { "ble-disconnect", prim_disconnect },
        { "ble-connections", prim_connections }, { "ble-discover", prim_discover },
        { "ble-services", prim_services }, { "ble-read", prim_read }, { "ble-write", prim_write },
        { "ble-subscribe", prim_subscribe }, { "ble-pair", prim_pair }, { "ble-bonds", prim_bonds },
        { "ble-unpair", prim_unpair }, { "ble-advertise", prim_advertise },
        { "ble-advertise-stop", prim_advertise_stop }, { "ble-uart-write", prim_uart_write },
        { "ble-bytes->hex", prim_bytes_hex }, { "ble-hex->bytes", prim_hex_bytes },
    };
    mini_register_prim_table(vm, PRIMS, sizeof(PRIMS) / sizeof(PRIMS[0]));
}

/* ------------------------------------------------------------------------- */
/* Start                                                                     */
/* ------------------------------------------------------------------------- */

static void on_sync(void) {
    if (ble_hs_util_ensure_addr(0) != 0 || ble_hs_id_infer_auto(0, &s_own_addr_type) != 0) {
        ESP_LOGE(TAG, "no address");
        return;
    }
    s_synced = true;
    char addr[18];
    own_address(addr);
    ESP_LOGI(TAG, "ready: %s (%s)", addr, s_name);
}

static void on_reset(int reason) {
    s_synced = false;
    s_scanning = s_advertising = s_connecting = false;
    ESP_LOGW(TAG, "host reset: %d", reason);
}

static void host_task(void *arg) {
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_release_memory(void) {
#if CONFIG_IDF_TARGET_ESP32
    esp_bt_mem_release(ESP_BT_MODE_BTDM);
#else
    esp_bt_mem_release(ESP_BT_MODE_BLE);
#endif
}

bool ble_running(void) { return s_running; }
size_t ble_heap_used(void) { return s_heap_used; }

bool ble_start(const char *name) {
    if (s_running) return true;
    size_t before = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    if (name && name[0]) snprintf(s_name, sizeof(s_name), "%s", name);
#if CONFIG_IDF_TARGET_ESP32
    esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);   /* BLE only (a dual-mode controller) */
#endif
    s_lock = xSemaphoreCreateMutex();
    s_events = (BleEvent *)calloc(BLE_EVENT_MAX, sizeof(BleEvent));
    s_seen = (BleSeen *)calloc(BLE_SCAN_MAX, sizeof(BleSeen));
    if (!s_lock || !s_events || !s_seen) {
        ESP_LOGE(TAG, "out of memory");
        return false;
    }
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(err));
        return false;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
#if BLE_SECURITY
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;   /* just works: the board has no keypad for a passkey */
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
#endif
    ble_svc_gap_init();
    ble_svc_gatt_init();
    if (ble_gatts_count_cfg(GATT_SERVICES) != 0 || ble_gatts_add_svcs(GATT_SERVICES) != 0) {
        ESP_LOGE(TAG, "the UART service could not be registered");
    }
    ble_svc_gap_device_name_set(s_name);
#if BLE_SECURITY
    ble_store_config_init();
#endif
    nimble_port_freertos_init(host_task);
    s_running = true;
    size_t after = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    s_heap_used = before > after ? before - after : 0;
    return true;
}
