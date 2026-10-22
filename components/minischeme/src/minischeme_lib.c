/**
 * @file minischeme_lib.c
 * @brief MiniScheme Hardware Abstraction, Mock Simulator, and REPLs.
 */

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include "minischeme_internal.h"
#include "minischeme_lib.h"
#include "minischeme_api.h"
#include "minischeme_device.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#include <conio.h>
#include <io.h>
#define IS_TTY() _isatty(_fileno(stdin))
#elif defined(ESP_PLATFORM) || defined(ARDUINO)
#include <time.h>
#include <unistd.h>
#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#include "esp_log.h"
#endif
#define IS_TTY() 0
#else
#include <time.h>
#include <unistd.h>
#include <termios.h>
#define IS_TTY() isatty(fileno(stdin))
#endif

/* ========================================================================= */
/* Section 1: PC Mock Simulator Layer                                        */
/* ========================================================================= */

#if !defined(ARDUINO) && !defined(ESP_PLATFORM)

#define MOCK_MAX_PINS 64
#define MOCK_MAX_FILES 32

typedef struct {
    int mode;
    int level;
    int analog;
    int pwm_duty;
    int pwm_freq;
} MockPin;

typedef struct {
    char path[128];
    char *content;
    size_t len;
    bool in_use;
} MockFile;

/* Mock Hardware State */
static MockPin  s_mock_pins[MOCK_MAX_PINS];
static MockFile s_mock_files[MOCK_MAX_FILES];
static bool     s_mock_wifi_connected = false;
static char     s_mock_wifi_ip[32] = "0.0.0.0";
static char     s_mock_wifi_ssid[64] = "";
static uint32_t s_mock_manual_time_ms = 0;
static bool     s_mock_use_manual_time = false;

/* Mock GPIO Accessors */
int mini_mock_get_pin_mode(int pin) {
    if (pin >= 0 && pin < MOCK_MAX_PINS) {
        return s_mock_pins[pin].mode;
    }
    return -1;
}

int mini_mock_get_pin_level(int pin) {
    if (pin >= 0 && pin < MOCK_MAX_PINS) {
        return s_mock_pins[pin].level;
    }
    return 0;
}

void mini_mock_set_pin_level(int pin, int level) {
    if (pin >= 0 && pin < MOCK_MAX_PINS) {
        s_mock_pins[pin].level = level;
    }
}

int mini_mock_get_pin_analog(int pin) {
    if (pin >= 0 && pin < MOCK_MAX_PINS) {
        return s_mock_pins[pin].analog;
    }
    return 0;
}

void mini_mock_set_pin_analog(int pin, int val) {
    if (pin >= 0 && pin < MOCK_MAX_PINS) {
        s_mock_pins[pin].analog = val;
    }
}

int mini_mock_get_pin_pwm_duty(int pin) {
    if (pin >= 0 && pin < MOCK_MAX_PINS) {
        return s_mock_pins[pin].pwm_duty;
    }
    return 0;
}

int mini_mock_get_pin_pwm_freq(int pin) {
    if (pin >= 0 && pin < MOCK_MAX_PINS) {
        return s_mock_pins[pin].pwm_freq;
    }
    return 0;
}

/* Mock WiFi Accessors */
bool mini_mock_get_wifi_connected(void) {
    return s_mock_wifi_connected;
}

void mini_mock_set_wifi_connected(bool connected, const char *ip) {
    s_mock_wifi_connected = connected;
    if (ip) {
        strncpy(s_mock_wifi_ip, ip, sizeof(s_mock_wifi_ip) - 1);
        s_mock_wifi_ip[sizeof(s_mock_wifi_ip) - 1] = '\0';
    } else {
        strcpy(s_mock_wifi_ip, connected ? "192.168.4.1" : "0.0.0.0");
    }
}

const char *mini_mock_get_wifi_ip(void) {
    return s_mock_wifi_ip;
}

const char *mini_mock_get_wifi_ssid(void) {
    return s_mock_wifi_ssid;
}

/* Mock Virtual File System Accessors */
void mini_mock_reset_fs(void) {
    for (int i = 0; i < MOCK_MAX_FILES; i++) {
        if (s_mock_files[i].in_use && s_mock_files[i].content) {
            free(s_mock_files[i].content);
        }
        s_mock_files[i].content = NULL;
        s_mock_files[i].in_use = false;
        s_mock_files[i].path[0] = '\0';
    }
}

/* Binary-safe: len bytes (NUL-terminated copy); false when the table is full */
static bool mock_fs_put(const char *path, const char *data, size_t len) {
    if (!path || !data) return false;
    int slot = -1;
    for (int i = 0; i < MOCK_MAX_FILES && slot < 0; i++) {
        if (s_mock_files[i].in_use && strcmp(s_mock_files[i].path, path) == 0) slot = i;
    }
    for (int i = 0; i < MOCK_MAX_FILES && slot < 0; i++) {
        if (!s_mock_files[i].in_use) slot = i;
    }
    if (slot < 0) return false;
    char *copy = (char *)malloc(len + 1);
    if (!copy) return false;
    memcpy(copy, data, len);
    copy[len] = '\0';
    if (s_mock_files[slot].in_use) free(s_mock_files[slot].content);
    s_mock_files[slot].in_use = true;
    strncpy(s_mock_files[slot].path, path, sizeof(s_mock_files[slot].path) - 1);
    s_mock_files[slot].path[sizeof(s_mock_files[slot].path) - 1] = '\0';
    s_mock_files[slot].content = copy;
    s_mock_files[slot].len = len;
    return true;
}

void mini_mock_add_fs_file(const char *path, const char *content) {
    if (content) mock_fs_put(path, content, strlen(content));
}

const char *mini_mock_get_fs_file(const char *path) {
    if (!path) return NULL;
    for (int i = 0; i < MOCK_MAX_FILES; i++) {
        if (s_mock_files[i].in_use && strcmp(s_mock_files[i].path, path) == 0) {
            return s_mock_files[i].content;
        }
    }
    return NULL;
}

bool mini_mock_fs_exists(const char *path) {
    return mini_mock_get_fs_file(path) != NULL;
}

/* Mock Monotonic Clock Controls */
void mini_mock_set_millis(uint32_t ms) {
    s_mock_manual_time_ms = ms;
    s_mock_use_manual_time = true;
}

void mini_mock_add_millis(uint32_t ms) {
    s_mock_manual_time_ms += ms;
}

void mini_mock_reset_all(void) {
    memset(s_mock_pins, 0, sizeof(s_mock_pins));
    mini_mock_reset_fs();
    s_mock_wifi_connected = false;
    strcpy(s_mock_wifi_ip, "0.0.0.0");
    s_mock_wifi_ssid[0] = '\0';
    s_mock_manual_time_ms = 0;
    s_mock_use_manual_time = false;
}

/* Mock HAL Implementation */
static void hal_pin_mode(int pin, int mode) {
    if (pin >= 0 && pin < MOCK_MAX_PINS) {
        s_mock_pins[pin].mode = mode;
    }
}

static void hal_digital_write(int pin, int level) {
    if (pin >= 0 && pin < MOCK_MAX_PINS) {
        s_mock_pins[pin].level = level ? 1 : 0;
    }
}

static int hal_digital_read(int pin) {
    if (pin >= 0 && pin < MOCK_MAX_PINS) {
        return s_mock_pins[pin].level;
    }
    return 0;
}

static int hal_analog_read(int pin) {
    if (pin >= 0 && pin < MOCK_MAX_PINS) {
        return s_mock_pins[pin].analog;
    }
    return 0;
}

static void hal_pwm_write(int pin, int freq, int duty) {
    if (pin >= 0 && pin < MOCK_MAX_PINS) {
        s_mock_pins[pin].pwm_freq = freq;
        s_mock_pins[pin].pwm_duty = duty;
    }
}

static uint64_t hal_millis(void) {
    if (s_mock_use_manual_time) return s_mock_manual_time_ms;
#if defined(_WIN32)
    LARGE_INTEGER freq, counter;   /* not GetTickCount: it wraps after 49 days */
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return (uint64_t)(counter.QuadPart / freq.QuadPart) * 1000u +
           (uint64_t)((counter.QuadPart % freq.QuadPart) * 1000LL / freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}

static uint64_t hal_micros(void) {
    if (s_mock_use_manual_time) return (uint64_t)s_mock_manual_time_ms * 1000u;
#if defined(_WIN32)
    LARGE_INTEGER freq, counter;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return (uint64_t)(counter.QuadPart / freq.QuadPart) * 1000000u +
           (uint64_t)((counter.QuadPart % freq.QuadPart) * 1000000LL / freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
#endif
}

static void hal_delay_ms(uint32_t ms) {
    if (s_mock_use_manual_time) {
        s_mock_manual_time_ms += ms;
        return;
    }
#if defined(_WIN32)
    Sleep(ms);
#else
    usleep(ms * 1000);
#endif
}

static void hal_delay_us(uint32_t us) {
    if (s_mock_use_manual_time) {
        s_mock_manual_time_ms += (us / 1000);
        return;
    }
#if defined(_WIN32)
    Sleep((us + 999) / 1000);
#else
    usleep(us);
#endif
}

static bool hal_wifi_connect(const char *ssid, const char *pass) {
    (void)pass;
    strncpy(s_mock_wifi_ssid, ssid ? ssid : "", sizeof(s_mock_wifi_ssid) - 1);
    s_mock_wifi_connected = true;
    strcpy(s_mock_wifi_ip, "192.168.1.105");
    return true;
}

static bool hal_wifi_ap(const char *ssid, const char *pass) {
    (void)pass;
    strncpy(s_mock_wifi_ssid, ssid ? ssid : "", sizeof(s_mock_wifi_ssid) - 1);
    s_mock_wifi_connected = true;
    strcpy(s_mock_wifi_ip, "192.168.4.1");
    return true;
}

static bool hal_wifi_connected(void) {
    return s_mock_wifi_connected;
}

static const char *hal_wifi_ip(void) {
    return s_mock_wifi_ip;
}

static void hal_wifi_disconnect(void) {
    s_mock_wifi_connected = false;
    strcpy(s_mock_wifi_ip, "0.0.0.0");
    s_mock_wifi_ssid[0] = '\0';
}

static const char *hal_wifi_ssid(void) { return s_mock_wifi_ssid; }
static int hal_wifi_rssi(void) { return s_mock_wifi_connected ? -52 : 0; }
/* Host clock is assumed to be correct */
static bool hal_time_synced(void) { return true; }

static bool hal_fs_write(const char *path, const char *data, size_t len) {
    return mock_fs_put(path, data, len);
}

/* The file's bytes (malloc'd, NUL-terminated) and their count */
static char *hal_fs_read(const char *path, size_t *len) {
    for (int i = 0; path && i < MOCK_MAX_FILES; i++) {
        if (s_mock_files[i].in_use && strcmp(s_mock_files[i].path, path) == 0) {
            char *res = (char *)malloc(s_mock_files[i].len + 1);
            if (!res) return NULL;
            memcpy(res, s_mock_files[i].content, s_mock_files[i].len + 1);
            *len = s_mock_files[i].len;
            return res;
        }
    }
    return NULL;
}

static bool hal_fs_exists(const char *path) {
    return mini_mock_fs_exists(path);
}

static bool hal_fs_remove(const char *path) {
    if (!path) return false;
    for (int i = 0; i < MOCK_MAX_FILES; i++) {
        if (s_mock_files[i].in_use && strcmp(s_mock_files[i].path, path) == 0) {
            free(s_mock_files[i].content);
            s_mock_files[i].content = NULL;
            s_mock_files[i].in_use = false;
            s_mock_files[i].path[0] = '\0';
            return true;
        }
    }
    return false;
}

static size_t hal_free_heap(void) {
    return 1024 * 1024 * 4;
}

static void hal_restart(void) {
    mini_mock_reset_all();
}

static void hal_deep_sleep(uint32_t ms) {
    hal_delay_ms(ms);
}

static const char *hal_chip_model(void) {
    return "PC-Mock-ESP32-Virtual";
}

#elif defined(ARDUINO)

/* ========================================================================= */
/* Section 2: Arduino C++ Implementation                                     */
/* ========================================================================= */

#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <esp_system.h>
#include <esp_sleep.h>

static void hal_pin_mode(int pin, int mode) {
    int m = OUTPUT;
    if (mode == MINI_PIN_INPUT) m = INPUT;
    else if (mode == MINI_PIN_INPUT_PULLUP) m = INPUT_PULLUP;
    pinMode(pin, m);
}

static void hal_digital_write(int pin, int level) {
    digitalWrite(pin, level ? HIGH : LOW);
}

static int hal_digital_read(int pin) {
    return digitalRead(pin) == HIGH ? 1 : 0;
}

static int hal_analog_read(int pin) {
    return analogRead(pin);
}

static void hal_pwm_write(int pin, int freq, int duty) {
#if defined(ESP32)
    /* Arduino ESP32 v3.x uses ledcAttachChannel / ledcWrite */
    ledcAttach(pin, freq > 0 ? freq : 5000, 8);
    ledcWrite(pin, duty);
#else
    analogWrite(pin, duty);
#endif
}

static uint64_t hal_millis(void) {
    return millis();
}

static uint64_t hal_micros(void) {
    return micros();
}

static void hal_delay_ms(uint32_t ms) {
    delay(ms);
}

static void hal_delay_us(uint32_t us) {
    delayMicroseconds(us);
}

static bool hal_wifi_connect(const char *ssid, const char *pass) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pass);
    int retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 20) {
        delay(250);
        retries++;
    }
    return WiFi.status() == WL_CONNECTED;
}

static bool hal_wifi_ap(const char *ssid, const char *pass) { WiFi.mode(WIFI_AP); return WiFi.softAP(ssid, pass); }
static bool hal_wifi_connected(void) { return WiFi.status() == WL_CONNECTED; }
static const char *hal_wifi_ip(void) {
    static char ip_buf[32];
    const char *s = (WiFi.getMode() == WIFI_AP) ? WiFi.softAPIP().toString().c_str() : WiFi.localIP().toString().c_str();
    strncpy(ip_buf, s, sizeof(ip_buf) - 1); ip_buf[sizeof(ip_buf) - 1] = '\0';
    return ip_buf;
}
static void hal_wifi_disconnect(void) { WiFi.disconnect(true); }
static const char *hal_wifi_ssid(void) {
    static char ssid[33];
    strncpy(ssid, WiFi.SSID().c_str(), sizeof(ssid) - 1);
    ssid[sizeof(ssid) - 1] = '\0';
    return ssid;
}
static int hal_wifi_rssi(void) { return WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0; }
static bool hal_time_synced(void) { return time(NULL) > 1700000000; }

static bool hal_fs_write(const char *path, const char *data, size_t len) {
    if (!LittleFS.begin(true)) return false;
    File f = LittleFS.open(path, "w");
    if (!f) return false;
    size_t w = f.write((const uint8_t *)data, len);
    f.close();
    return w == len;
}

static char *hal_fs_read(const char *path, size_t *len) {
    if (!LittleFS.begin(true)) return NULL;
    File f = LittleFS.open(path, "r");
    if (!f) return NULL;
    size_t sz = f.size();
    char *buf = (char *)malloc(sz + 1);
    if (!buf) {
        f.close();
        return NULL;
    }
    *len = f.read((uint8_t *)buf, sz);
    buf[*len] = '\0';
    f.close();
    return buf;
}

static bool hal_fs_exists(const char *path) {
    if (!LittleFS.begin(true)) return false;
    return LittleFS.exists(path);
}

static bool hal_fs_remove(const char *path) {
    if (!LittleFS.begin(true)) return false;
    return LittleFS.remove(path);
}

static size_t hal_free_heap(void) {
    return ESP.getFreeHeap();
}

static void hal_restart(void) {
    ESP.restart();
}

static void hal_deep_sleep(uint32_t ms) {
    esp_deep_sleep(ms * 1000ULL);
}

static const char *hal_chip_model(void) {
    return ESP.getChipModel();
}

#elif defined(ESP_PLATFORM)

/* ========================================================================= */
/* Section 3: ESP-IDF Native C Implementation                                */
/* ========================================================================= */

#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_sleep.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <time.h>
#include <sys/time.h>
#include "esp_chip_info.h"
#include "esp_idf_version.h"
#include "esp_mac.h"
#include "esp_flash.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "esp_image_format.h"
#include "esp_clk_tree.h"

static bool (*s_storage_info)(size_t *total, size_t *used) = NULL;

void esp32_set_storage_info(bool (*info)(size_t *total, size_t *used)) {
    s_storage_info = info;
}

static inline bool is_display_bus_pin(int pin) {
    return (pin == 2 || pin == 4 || pin == 15 || pin == 32 || pin == 33 ||
            pin == 12 || pin == 13 || pin == 26 || pin == 25 ||
            pin == 17 || pin == 16 || pin == 27 || pin == 14);
}

static void hal_pin_mode(int pin, int mode) {
    if (is_display_bus_pin(pin)) {
        return;
    }
    gpio_config_t io_conf = {0};
    io_conf.pin_bit_mask = (1ULL << pin);
    if (mode == 0) {
        io_conf.mode = GPIO_MODE_INPUT;
    } else if (mode == 2) {
        io_conf.mode = GPIO_MODE_INPUT;
        io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    } else {
        io_conf.mode = GPIO_MODE_INPUT_OUTPUT;   /* (gpio-read pin) reads back the level written */
    }
    gpio_config(&io_conf);
}

static void hal_digital_write(int pin, int level) {
    if (is_display_bus_pin(pin)) {
        return;
    }
    gpio_set_level((gpio_num_t)pin, level ? 1 : 0);
}

static int hal_digital_read(int pin) {
    return gpio_get_level((gpio_num_t)pin);
}

/* the board driver (minischeme_ili9488.c) */
int esp32_adc_read(int pin);
bool esp32_pwm_write(int pin, int freq_hz, int duty);
void mini_backlight_set(uint8_t level);

/* ADC1 (GPIO 32..39), 12 bits; 0 for other pins */
static int hal_analog_read(int pin) {
    int v = esp32_adc_read(pin);
    return v < 0 ? 0 : v;
}

/* LEDC PWM, 8-bit duty; GPIO 22 is the backlight's own channel (as the simulator) */
static void hal_pwm_write(int pin, int freq, int duty) {
    if (pin == 22) {
        mini_backlight_set((uint8_t)(duty < 0 ? 0 : (duty > 255 ? 255 : duty)));
        return;
    }
    if (!is_display_bus_pin(pin)) esp32_pwm_write(pin, freq, duty);
}

static uint64_t hal_millis(void) {
    return (uint64_t)esp_timer_get_time() / 1000u;
}

static uint64_t hal_micros(void) {
    return (uint64_t)esp_timer_get_time();
}

static void hal_delay_ms(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static void hal_delay_us(uint32_t us) {
    esp_rom_delay_us(us);
}

static bool s_esp32_wifi_inited = false;
static bool s_esp32_wifi_connected = false;
static char s_esp32_wifi_ip[32] = "0.0.0.0";
/* Auto-reconnect after a dropped link, backing off 0.5 s .. 30 s; an
 * explicit (wifi-disconnect) turns it off until the next (wifi-connect) */
static bool s_esp32_wifi_auto = false;
static uint32_t s_esp32_wifi_backoff_ms = 500;
static esp_timer_handle_t s_esp32_wifi_retry_timer = NULL;
static char s_esp32_wifi_ssid[33] = "";
static esp_netif_t *s_esp32_sta_netif = NULL;

#if defined(ESP_PLATFORM)
void esp32_sntp_init(void) {
    if (!esp_sntp_enabled()) {
        ESP_LOGI("MiniScheme_SNTP", "Initializing SNTP client (ntp.aliyun.com, cn.pool.ntp.org)...");
        esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "ntp.aliyun.com");
        esp_sntp_setservername(1, "cn.pool.ntp.org");
        esp_sntp_setservername(2, "pool.ntp.org");
        esp_sntp_init();
        setenv("TZ", "CST-8", 1);
        tzset();
    }
}

static void ip_geo_task(void *pvParameters) {
    (void)pvParameters;
    vTaskDelay(pdMS_TO_TICKS(1500));
    struct hostent *he = gethostbyname("myip.ipip.net");
    if (he && he->h_addr_list && he->h_addr_list[0]) {
        int s = socket(AF_INET, SOCK_STREAM, 0);
        if (s >= 0) {
            struct timeval tv;
            tv.tv_sec = 3;
            tv.tv_usec = 0;
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            struct sockaddr_in dest;
            memset(&dest, 0, sizeof(dest));
            dest.sin_family = AF_INET;
            dest.sin_port = htons(80);
            dest.sin_addr.s_addr = *(in_addr_t *)he->h_addr_list[0];
            if (connect(s, (struct sockaddr *)&dest, sizeof(dest)) == 0) {
                const char *req = "GET / HTTP/1.1\r\nHost: myip.ipip.net\r\nUser-Agent: curl/7.79.1\r\nConnection: close\r\n\r\n";
                send(s, req, strlen(req), 0);
                char buf[256];
                int n = recv(s, buf, sizeof(buf) - 1, 0);
                if (n > 0) {
                    buf[n] = '\0';
                    ESP_LOGI("MiniScheme_Geo", "IP location report: %s", buf);
                    if (strstr(buf, "\xe4\xb8\xad\xe5\x9b\xbd") || strstr(buf, "China")) {
                        setenv("TZ", "CST-8", 1);
                        tzset();
                        ESP_LOGI("MiniScheme_Geo", "Timezone automatically selected: CST-8 (Asia/Shanghai)");
                    }
                }
            }
            close(s);
        }
    }
    vTaskDelete(NULL);
}
#endif

/* A watchdog for the station: the reconnect chain above runs on the
 * driver's events, and when none comes (an association that never gets an
 * address, a connect call that failed without an event, a router still
 * starting after a power cut) the attempt starts over. Connected with the
 * clock still unset, SNTP asks again at once (its retry delay grew while
 * offline). */
#define ESP32_WIFI_STALL_US (45LL * 1000000)   /* no address and no driver event: connect again */
#define ESP32_SNTP_KICK_US  (60LL * 1000000)   /* connected, clock unset: SNTP again */
static int64_t s_esp32_wifi_last_us = 0;      /* the last connect attempt or driver event */
static int64_t s_esp32_sntp_kick_us = 0;
static uint32_t s_esp32_wifi_fails = 0;       /* failed attempts since the last connection */
static esp_timer_handle_t s_esp32_wifi_watchdog_timer = NULL;

static void esp32_wifi_retry(void *arg) {
    (void)arg;
    if (s_esp32_wifi_auto && !s_esp32_wifi_connected) {
        s_esp32_wifi_last_us = esp_timer_get_time();
        esp_wifi_connect();
    }
}

static void esp32_wifi_watchdog(void *arg) {
    (void)arg;
    int64_t now = esp_timer_get_time();
    if (s_esp32_wifi_auto && !s_esp32_wifi_connected) {
        if (now - s_esp32_wifi_last_us > ESP32_WIFI_STALL_US) {
            ESP_LOGW("MiniScheme_WiFi", "No connection for %lld s: connecting again",
                     (long long)((now - s_esp32_wifi_last_us) / 1000000));
            s_esp32_wifi_last_us = now;
            s_esp32_wifi_backoff_ms = 500;
            esp_wifi_disconnect();
            esp_wifi_connect();
        }
    } else if (s_esp32_wifi_connected && !hal_time_synced() && now - s_esp32_sntp_kick_us > ESP32_SNTP_KICK_US) {
        s_esp32_sntp_kick_us = now;
        ESP_LOGW("MiniScheme_SNTP", "Clock not set yet: asking the time servers again");
        esp_sntp_restart();
    }
}

static void esp32_wifi_watchdog_start(void) {
    if (s_esp32_wifi_watchdog_timer) return;
    const esp_timer_create_args_t args = { .callback = esp32_wifi_watchdog, .name = "wifi_watchdog" };
    if (esp_timer_create(&args, &s_esp32_wifi_watchdog_timer) == ESP_OK) {
        esp_timer_start_periodic(s_esp32_wifi_watchdog_timer, 10LL * 1000000);
    }
}

static void esp32_wifi_event_handler(void* arg, esp_event_base_t event_base,
                                     int32_t event_id, void* event_data) {
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        bool was_connected = s_esp32_wifi_connected;
        s_esp32_wifi_connected = false;
        strcpy(s_esp32_wifi_ip, "0.0.0.0");
        const wifi_event_sta_disconnected_t *d = (const wifi_event_sta_disconnected_t *)event_data;
        s_esp32_wifi_last_us = esp_timer_get_time();
        if (was_connected) {
            s_esp32_wifi_fails = 0;
            ESP_LOGW("MiniScheme_WiFi", "WiFi disconnected (reason %d)", d ? (int)d->reason : -1);
        } else if (++s_esp32_wifi_fails <= 3 || s_esp32_wifi_fails % 20 == 0) {
            ESP_LOGW("MiniScheme_WiFi", "WiFi connect failed (reason %d, attempt %u)",
                     d ? (int)d->reason : -1, (unsigned)s_esp32_wifi_fails);
        }
        if (s_esp32_wifi_auto) {
            if (!s_esp32_wifi_retry_timer) {
                const esp_timer_create_args_t args = { .callback = esp32_wifi_retry, .name = "wifi_retry" };
                esp_timer_create(&args, &s_esp32_wifi_retry_timer);
            }
            if (s_esp32_wifi_retry_timer) {
                esp_timer_stop(s_esp32_wifi_retry_timer);
                esp_timer_start_once(s_esp32_wifi_retry_timer, (uint64_t)s_esp32_wifi_backoff_ms * 1000);
            }
            s_esp32_wifi_backoff_ms = s_esp32_wifi_backoff_ms >= 15000 ? 30000 : s_esp32_wifi_backoff_ms * 2;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_esp32_wifi_backoff_ms = 500;
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        esp_ip4addr_ntoa(&event->ip_info.ip, s_esp32_wifi_ip, sizeof(s_esp32_wifi_ip));
        s_esp32_wifi_connected = true;
        s_esp32_wifi_fails = 0;
        ESP_LOGI("MiniScheme_WiFi", "WiFi connected, IP: %s", s_esp32_wifi_ip);
#if defined(ESP_PLATFORM)
        if (esp_sntp_enabled() && !hal_time_synced()) {   /* at once, not after the offline backoff */
            s_esp32_sntp_kick_us = esp_timer_get_time();
            esp_sntp_restart();
        }
        esp32_sntp_init();
        xTaskCreate(ip_geo_task, "ip_geo_task", 3072, NULL, 3, NULL);
#endif
    }
}

#if defined(ESP_PLATFORM)
void esp32_wifi_early_init(void) {
    if (!s_esp32_wifi_inited) {
        if (!s_esp32_sta_netif) {
            esp_netif_init();
            esp_event_loop_create_default();
            s_esp32_sta_netif = esp_netif_create_default_wifi_sta();
        }
        /* the buffer counts of sdkconfig (2 static, 6 dynamic rx, 6 tx): each
         * static rx buffer holds 1.6 KB for good, the VM heap gets it instead */
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        cfg.mgmt_sbuf_num = 8;
        cfg.rx_ba_win = 4;
        esp_err_t err = esp_wifi_init(&cfg);
        if (err == ESP_OK) {
            esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &esp32_wifi_event_handler, NULL, NULL);
            esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &esp32_wifi_event_handler, NULL, NULL);
            esp_wifi_set_mode(WIFI_MODE_STA);
            esp_wifi_start();
            esp_wifi_set_ps(WIFI_PS_NONE);
            s_esp32_wifi_inited = true;
        } else {
            ESP_LOGE("MiniScheme_WiFi", "esp_wifi_init failed: %d", (int)err);
        }
    }
}
#endif

bool hal_wifi_connect(const char *ssid, const char *pass) {
    if (!ssid || strlen(ssid) == 0) return false;
    if (!s_esp32_wifi_inited) {
        esp32_wifi_early_init();
        if (!s_esp32_wifi_inited) return false;
    }
    wifi_config_t wifi_config;
    memset(&wifi_config, 0, sizeof(wifi_config));
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy(s_esp32_wifi_ssid, ssid, sizeof(s_esp32_wifi_ssid) - 1);
    if (pass) {
        strncpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password) - 1);
    }
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    s_esp32_wifi_auto = true;
    s_esp32_wifi_backoff_ms = 500;
    s_esp32_wifi_last_us = esp_timer_get_time();
    esp32_wifi_watchdog_start();
    esp_wifi_connect();
    return true;
}

static esp_netif_t *s_esp32_ap_netif = NULL;

bool hal_wifi_ap(const char *ssid, const char *pass) {
    if (!ssid || !ssid[0]) return false;
    if (!s_esp32_wifi_inited) {
        esp32_wifi_early_init();
        if (!s_esp32_wifi_inited) return false;
    }
    if (!s_esp32_ap_netif) s_esp32_ap_netif = esp_netif_create_default_wifi_ap();
    if (!s_esp32_ap_netif) return false;
    wifi_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    size_t n = strlen(ssid);
    if (n > sizeof(cfg.ap.ssid)) n = sizeof(cfg.ap.ssid);
    memcpy(cfg.ap.ssid, ssid, n);
    cfg.ap.ssid_len = (uint8_t)n;
    cfg.ap.channel = 1;          /* follows the station's channel once it is connected */
    cfg.ap.max_connection = 4;
    if (pass && strlen(pass) >= 8) {
        strncpy((char *)cfg.ap.password, pass, sizeof(cfg.ap.password) - 1);
        cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        cfg.ap.authmode = WIFI_AUTH_OPEN;
    }
    if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK) return false;
    return esp_wifi_set_config(WIFI_IF_AP, &cfg) == ESP_OK;
}

void esp32_wifi_pause_station(void) {
    if (s_esp32_wifi_connected) return;
    s_esp32_wifi_auto = false;
    if (s_esp32_wifi_retry_timer) esp_timer_stop(s_esp32_wifi_retry_timer);
    if (s_esp32_wifi_inited) esp_wifi_disconnect();
}

static bool hal_wifi_connected(void) {
    return s_esp32_wifi_connected;
}

bool esp32_wifi_connected(void) {
    return s_esp32_wifi_connected;
}

static const char *hal_wifi_ip(void) {
    return s_esp32_wifi_ip;
}

/* (wifi-disconnect #t) bounces the link: the auto-reconnect stays on */
static bool s_wifi_disconnect_keep_auto = false;

static void hal_wifi_disconnect(void) {
    s_esp32_wifi_auto = s_wifi_disconnect_keep_auto;
    if (s_esp32_wifi_inited) {
        esp_wifi_disconnect();
    }
    s_esp32_wifi_connected = false;
    strcpy(s_esp32_wifi_ip, "0.0.0.0");
}

static const char *hal_wifi_ssid(void) {
    return s_esp32_wifi_connected ? s_esp32_wifi_ssid : "";
}

static int hal_wifi_rssi(void) {
    wifi_ap_record_t ap;
    if (s_esp32_wifi_connected && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) return ap.rssi;
    return 0;
}

bool hal_time_synced(void) {
    /* SNTP sets the RTC; before that the epoch starts at 1970 */
    return time(NULL) > 1700000000;
}

static bool hal_fs_write(const char *path, const char *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    size_t w = fwrite(data, 1, len, f);
    fclose(f);
    return w == len;
}

static char *hal_fs_read(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    *len = fread(buf, 1, (size_t)sz, f);
    buf[*len] = '\0';
    fclose(f);
    return buf;
}

static bool hal_fs_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f) { fclose(f); return true; }
    return false;
}

static bool hal_fs_remove(const char *path) {
    return remove(path) == 0;
}

static size_t hal_free_heap(void) {
    return esp_get_free_heap_size();
}

static void hal_restart(void) {
    esp_restart();
}

static void hal_deep_sleep(uint32_t ms) {
    esp_deep_sleep(ms * 1000ULL);
}

static const char *hal_chip_model(void) {
    static char model[32];
    esp_chip_info_t info;
    esp_chip_info(&info);
    snprintf(model, sizeof(model), "ESP32 rev%d.%d %dC", info.revision / 100, info.revision % 100, info.cores);
    return model;
}

#endif /* Platforms */

/* ========================================================================= */
/* Section 4: Scheme Primitives Wrapper Bindings                             */
/* ========================================================================= */

static Value prim_pin_mode(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    if (argc < 2 || !mini_is_fixnum(argv[0])) return mini_void();
    int pin = (int)mini_to_fixnum(argv[0]);
    int mode = 1;
    if (mini_is_fixnum(argv[1])) {
        mode = (int)mini_to_fixnum(argv[1]);
    } else if (mini_is_symbol(argv[1])) {
        const char *s = mini_symbol_name(argv[1]);
        if (strcmp(s, "input") == 0) mode = 0;
        else if (strcmp(s, "output") == 0) mode = 1;
        else if (strcmp(s, "pullup") == 0 || strcmp(s, "input-pullup") == 0) mode = 2;
    }
    hal_pin_mode(pin, mode);
    return mini_void();
}

static Value prim_digital_write(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    if (argc >= 2 && mini_is_fixnum(argv[0]) && mini_is_fixnum(argv[1])) {
        hal_digital_write((int)mini_to_fixnum(argv[0]), (int)mini_to_fixnum(argv[1]));
    }
    return mini_void();
}

static Value prim_digital_read(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    if (argc >= 1 && mini_is_fixnum(argv[0])) {
        return mini_make_fixnum(hal_digital_read((int)mini_to_fixnum(argv[0])));
    }
    return mini_make_fixnum(0);
}

static Value prim_analog_read(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    if (argc >= 1 && mini_is_fixnum(argv[0])) {
        return mini_make_fixnum(hal_analog_read((int)mini_to_fixnum(argv[0])));
    }
    return mini_make_fixnum(0);
}

static Value prim_pwm_write(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    if (argc >= 3 && mini_is_fixnum(argv[0]) && mini_is_fixnum(argv[1]) && mini_is_fixnum(argv[2])) {
        hal_pwm_write((int)mini_to_fixnum(argv[0]), (int)mini_to_fixnum(argv[1]), (int)mini_to_fixnum(argv[2]));
    }
    return mini_void();
}

/* Past the fixnums (29 bits on 32-bit targets: 3.1 days of ms, 4.5 minutes
 * of us) the counts are bignums, never wrapped or negative */
static Value prim_millis(MiniVM *vm, int argc, Value *argv) {
    (void)argc;
    (void)argv;
    return bigint_from_int64(vm, (int64_t)hal_millis());
}

static Value prim_micros(MiniVM *vm, int argc, Value *argv) {
    (void)argc;
    (void)argv;
    return bigint_from_int64(vm, (int64_t)hal_micros());
}

static Value prim_delay_ms(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    if (argc >= 1 && mini_is_fixnum(argv[0]) && mini_to_fixnum(argv[0]) > 0) {
        hal_delay_ms((uint32_t)mini_to_fixnum(argv[0]));
    }
    return mini_void();
}

static Value prim_delay_us(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    if (argc >= 1 && mini_is_fixnum(argv[0]) && mini_to_fixnum(argv[0]) > 0) {
        hal_delay_us((uint32_t)mini_to_fixnum(argv[0]));
    }
    return mini_void();
}

static Value prim_wifi_connect(MiniVM *vm, int argc, Value *argv) {
    if (!mini_access_privileged(vm, "wifi-connect")) return mini_void();
    (void)vm;
    if (argc >= 2 && mini_is_string(argv[0]) && mini_is_string(argv[1])) {
        return mini_make_bool(hal_wifi_connect(mini_string_data(argv[0]), mini_string_data(argv[1])));
    }
    return mini_false();
}

static Value prim_wifi_ap(MiniVM *vm, int argc, Value *argv) {
    if (!mini_access_privileged(vm, "wifi-ap")) return mini_void();
    (void)vm;
    if (argc >= 2 && mini_is_string(argv[0]) && mini_is_string(argv[1])) {
        return mini_make_bool(hal_wifi_ap(mini_string_data(argv[0]), mini_string_data(argv[1])));
    }
    return mini_false();
}

static Value prim_wifi_connected_p(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    (void)argc;
    (void)argv;
    return mini_make_bool(hal_wifi_connected());
}

static Value prim_wifi_ip(MiniVM *vm, int argc, Value *argv) {
    (void)argc;
    (void)argv;
    return mini_make_string(vm, hal_wifi_ip());
}

static Value prim_wifi_ssid(MiniVM *vm, int argc, Value *argv) {
    (void)argc; (void)argv;
    return mini_make_string(vm, hal_wifi_ssid());
}

static Value prim_wifi_rssi(MiniVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc; (void)argv;
    return mini_make_fixnum(hal_wifi_rssi());
}

static Value prim_time_synced_p(MiniVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc; (void)argv;
    return mini_make_bool(hal_time_synced());
}

/* (wifi-disconnect [reconnect?]): with #t the link drops and reconnects */
static Value prim_wifi_disconnect(MiniVM *vm, int argc, Value *argv) {
    if (!mini_access_privileged(vm, "wifi-disconnect")) return mini_void();
    (void)vm;
#if defined(ESP_PLATFORM) && !defined(ARDUINO)
    s_wifi_disconnect_keep_auto = argc > 0 && mini_is_true(argv[0]);
#else
    (void)argc; (void)argv;
#endif
    hal_wifi_disconnect();
#if defined(ESP_PLATFORM) && !defined(ARDUINO)
    s_wifi_disconnect_keep_auto = false;
#endif
    return mini_void();
}

/* An app writes only in its folder and within its storage quota
 * (minischeme_device.h); without a sandbox the path is used as given */
static Value prim_fs_write(MiniVM *vm, int argc, Value *argv) {
    char path[96];
    if (argc >= 2 && mini_is_string(argv[0]) && mini_is_string(argv[1])) {
        if (!mini_access_file(vm, "fs-write", mini_string_data(argv[0]), true, path, sizeof(path))) return mini_void();
        size_t old = 0;
        char *prev = hal_fs_read(path, &old);
        free(prev);
        char why[96];
        if (!mini_app_storage_ok(mini_string_len(argv[1]), prev ? old : 0, why, sizeof(why))) {
            vm_error(vm, "fs-write: %s", why);
            return mini_void();
        }
        return mini_make_bool(hal_fs_write(path, mini_string_data(argv[1]), mini_string_len(argv[1])));
    }
    return mini_false();
}

static Value prim_fs_read(MiniVM *vm, int argc, Value *argv) {
    char path[96];
    if (argc < 1 || !mini_is_string(argv[0])) return mini_false();
    if (!mini_access_file(vm, "fs-read", mini_string_data(argv[0]), false, path, sizeof(path))) return mini_void();
    size_t len = 0;
    char *c = hal_fs_read(path, &len);
    if (!c) return mini_false();
    Value res = mini_make_string_len(vm, c, len);   /* binary-safe, as fs-write */
    free(c);
    return res;
}

static Value prim_fs_exists_p(MiniVM *vm, int argc, Value *argv) {
    char path[96];
    if (argc >= 1 && mini_is_string(argv[0])) {
        if (!mini_access_file(vm, "fs-exists?", mini_string_data(argv[0]), false, path, sizeof(path))) return mini_void();
        return mini_make_bool(hal_fs_exists(path));
    }
    return mini_false();
}

static Value prim_fs_remove(MiniVM *vm, int argc, Value *argv) {
    char path[96];
    if (argc >= 1 && mini_is_string(argv[0])) {
        if (!mini_access_file(vm, "fs-remove", mini_string_data(argv[0]), true, path, sizeof(path))) return mini_void();
        return mini_make_bool(hal_fs_remove(path));
    }
    return mini_false();
}

static Value prim_free_heap(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    (void)argc;
    (void)argv;
    return mini_make_fixnum((int64_t)hal_free_heap());
}

static Value prim_restart(MiniVM *vm, int argc, Value *argv) {
    if (!mini_access_privileged(vm, "restart")) return mini_void();
    (void)vm;
    (void)argc;
    (void)argv;
    hal_restart();
    return mini_void();
}

static Value prim_deep_sleep(MiniVM *vm, int argc, Value *argv) {
    if (!mini_access_privileged(vm, "deep-sleep")) return mini_void();
    (void)vm;
    if (argc >= 1 && mini_is_fixnum(argv[0]) && mini_to_fixnum(argv[0]) > 0) {
        hal_deep_sleep((uint32_t)mini_to_fixnum(argv[0]));
    }
    return mini_void();
}

static Value prim_chip_model(MiniVM *vm, int argc, Value *argv) {
    (void)argc;
    (void)argv;
    return mini_make_string(vm, hal_chip_model());
}

static Value prim_vm_allocated_bytes(MiniVM *vm, int argc, Value *argv) {
    (void)argc; (void)argv;
    return mini_make_fixnum((int64_t)mini_get_allocated_bytes(vm));
}

static Value prim_vm_heap_size(MiniVM *vm, int argc, Value *argv) {
    (void)argc; (void)argv;
    return mini_make_fixnum((int64_t)mini_get_heap_size(vm));
}

static Value prim_vm_stack_depth(MiniVM *vm, int argc, Value *argv) {
    (void)argc; (void)argv;
    int depth = (vm && vm->stack) ? (int)(vm->sp - vm->stack) : 0;
    return mini_make_fixnum((int64_t)depth);
}

static Value prim_vm_frame_depth(MiniVM *vm, int argc, Value *argv) {
    (void)argc; (void)argv;
    int frames = vm ? vm->call_frame_count : 0;
    return mini_make_fixnum((int64_t)frames);
}

static Value prim_max_alloc_heap(MiniVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc; (void)argv;
#if defined(ESP_PLATFORM)
    return mini_make_fixnum((int64_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
#else
    return mini_make_fixnum(28672);
#endif
}

/* (min-free-heap): the lowest free system heap since boot */
static Value prim_min_free_heap(MiniVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc; (void)argv;
#if defined(ESP_PLATFORM)
    return mini_make_fixnum((int64_t)esp_get_minimum_free_heap_size());
#else
    return mini_make_fixnum(12288);
#endif
}

/* (heap-total): the internal RAM the system heap manages */
static Value prim_heap_total(MiniVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc; (void)argv;
#if defined(ESP_PLATFORM)
    return mini_make_fixnum((int64_t)heap_caps_get_total_size(MALLOC_CAP_INTERNAL));
#else
    return mini_make_fixnum(316 * 1024);
#endif
}

/* Flash layout: firmware image size, app partition size, chip size */
#if defined(ESP_PLATFORM)
static uint32_t esp_app_image_bytes(void) {
    static uint32_t s_image_len = 0; /* verifying hashes the image: do it once */
    if (s_image_len == 0) {
        const esp_partition_t *part = esp_ota_get_running_partition();
        if (part) {
            esp_partition_pos_t pos = { .offset = part->address, .size = part->size };
            esp_image_metadata_t meta;
            if (esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &meta) == ESP_OK) s_image_len = meta.image_len;
        }
    }
    return s_image_len;
}

static uint32_t esp_app_partition_bytes(void) {
    const esp_partition_t *part = esp_ota_get_running_partition();
    return part ? part->size : 0;
}

static uint32_t esp_flash_chip_bytes(void) {
    uint32_t size = 0;
    esp_flash_get_size(NULL, &size);
    return size;
}
#endif

static Value prim_flash_app_bytes(MiniVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc; (void)argv;
#if defined(ESP_PLATFORM)
    return mini_make_fixnum((int64_t)esp_app_image_bytes());
#else
    return mini_make_fixnum(1078192);
#endif
}

static Value prim_flash_app_partition_bytes(MiniVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc; (void)argv;
#if defined(ESP_PLATFORM)
    return mini_make_fixnum((int64_t)esp_app_partition_bytes());
#else
    return mini_make_fixnum(1536 * 1024); /* the board's factory partition */
#endif
}

/* (app-rom-capacity): bytes of the approm partition that holds the app's
 * Code ROM image (vm-rom-bytes is the image itself); 0 when there is none.
 * The firmware sets it once the partition is found; hosts mirror the
 * board's 256 KB. */
static size_t s_app_rom_capacity = 256 * 1024;

void mini_lib_set_app_rom_capacity(size_t bytes) {
    s_app_rom_capacity = bytes;
}

static Value prim_app_rom_capacity(MiniVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc; (void)argv;
    return mini_make_fixnum((int64_t)s_app_rom_capacity);
}

static Value prim_flash_total_bytes(MiniVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc; (void)argv;
#if defined(ESP_PLATFORM)
    return mini_make_fixnum((int64_t)esp_flash_chip_bytes());
#else
    return mini_make_fixnum(4 * 1024 * 1024);
#endif
}

/* Appends (key . val) to the alist in *head; both values are rooted by caller */
static void alist_add(MiniVM *vm, Value *head, const char *key, Value val) {
    mini_push_root(vm, &val);
    Value k = mini_make_symbol(vm, key);
    mini_push_root(vm, &k);
    Value pair = mini_make_cons(vm, k, val);
    mini_push_root(vm, &pair);
    *head = mini_make_cons(vm, pair, *head);
    mini_pop_root(vm);
    mini_pop_root(vm);
    mini_pop_root(vm);
}

static Value alist_reverse(MiniVM *vm, Value l) {
    Value res = mini_nil();
    mini_push_root(vm, &l);
    mini_push_root(vm, &res);
    while (mini_is_pair(l)) {
        res = mini_make_cons(vm, mini_car(l), res);
        l = mini_cdr(l);
    }
    mini_pop_root(vm);
    mini_pop_root(vm);
    return res;
}

/* (chip-info): alist describing the SoC, firmware and flash */
static Value prim_chip_info(MiniVM *vm, int argc, Value *argv) {
    (void)argc; (void)argv;
    Value l = mini_nil();
    mini_push_root(vm, &l);
#if defined(ESP_PLATFORM)
    esp_chip_info_t info;
    esp_chip_info(&info);
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char mac_str[20];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    uint32_t cpu_hz = 0;
    esp_clk_tree_src_get_freq_hz(SOC_MOD_CLK_CPU, ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED, &cpu_hz);
    static const char *const reasons[] = { "unknown", "power-on", "external", "software", "panic",
                                           "int-watchdog", "task-watchdog", "watchdog", "deep-sleep",
                                           "brownout", "sdio" };
    int rr = (int)esp_reset_reason();
    const esp_app_desc_t *app = esp_app_get_description();
    alist_add(vm, &l, "model", mini_make_string(vm, "ESP32"));
    alist_add(vm, &l, "revision", mini_make_fixnum(info.revision));
    alist_add(vm, &l, "cores", mini_make_fixnum(info.cores));
    alist_add(vm, &l, "cpu-mhz", mini_make_fixnum((int64_t)(cpu_hz / 1000000)));
    alist_add(vm, &l, "wifi-bt", mini_make_string(vm, (info.features & CHIP_FEATURE_BT) ? "WiFi+BT" : "WiFi"));
    alist_add(vm, &l, "mac", mini_make_string(vm, mac_str));
    alist_add(vm, &l, "idf", mini_make_string(vm, esp_get_idf_version()));
    alist_add(vm, &l, "build", mini_make_string(vm, app ? app->date : ""));
    alist_add(vm, &l, "reset-reason", mini_make_string(vm, (rr >= 0 && rr < 11) ? reasons[rr] : "unknown"));
    alist_add(vm, &l, "flash-bytes", mini_make_fixnum((int64_t)esp_flash_chip_bytes()));
    alist_add(vm, &l, "app-image-bytes", mini_make_fixnum((int64_t)esp_app_image_bytes()));
    alist_add(vm, &l, "app-partition-bytes", mini_make_fixnum((int64_t)esp_app_partition_bytes()));
    size_t fs_total = 0, fs_used = 0;
    if (s_storage_info && s_storage_info(&fs_total, &fs_used)) {
        alist_add(vm, &l, "spiffs-bytes", mini_make_fixnum((int64_t)fs_total));
        alist_add(vm, &l, "spiffs-used", mini_make_fixnum((int64_t)fs_used));
    }
#else
    alist_add(vm, &l, "model", mini_make_string(vm, hal_chip_model()));
    alist_add(vm, &l, "cores", mini_make_fixnum(1));
    alist_add(vm, &l, "flash-bytes", mini_make_fixnum(4 * 1024 * 1024));
#endif
    Value res = alist_reverse(vm, l);
    mini_pop_root(vm);
    return res;
}

/* ========================================================================= */
/* Console & Interactive Line Reader Library                                 */
/* ========================================================================= */

#define MAX_HISTORY 256
static char **s_history = NULL; /* allocated on first entry */
static int s_history_count = 0;

void mini_console_history_add(const char *cmd) {
    if (!cmd) return;
    while (isspace((unsigned char)*cmd)) cmd++;
    size_t len = strlen(cmd);
    while (len > 0 && isspace((unsigned char)cmd[len - 1])) len--;
    if (len == 0) return;

    char *entry = (char *)malloc(len + 1);
    if (!entry) return;

    bool in_str = false, in_cmt = false;
    size_t out_len = 0;
    for (size_t i = 0; i < len; i++) {
        char c = cmd[i];
        if (in_cmt) { if (c == '\n' || c == '\r') in_cmt = false; continue; }
        if (in_str) {
            if (c == '"' && (i == 0 || cmd[i - 1] != '\\')) in_str = false;
            entry[out_len++] = c; continue;
        }
        if (c == ';') { in_cmt = true; continue; }
        if (c == '"') { in_str = true; entry[out_len++] = c; continue; }
        if (c == '\n' || c == '\r') {
            if (out_len > 0 && entry[out_len - 1] != ' ') entry[out_len++] = ' ';
            continue;
        }
        entry[out_len++] = c;
    }
    while (out_len > 0 && isspace((unsigned char)entry[out_len - 1])) out_len--;
    entry[out_len] = '\0';

    if (out_len == 0 || (s_history_count > 0 && strcmp(s_history[s_history_count - 1], entry) == 0)) {
        free(entry); return;
    }
    if (!s_history) s_history = (char **)calloc(MAX_HISTORY, sizeof(char *));
    if (!s_history) {
        free(entry);
        return;
    }
    if (s_history_count < MAX_HISTORY) {
        s_history[s_history_count++] = entry;
    } else {
        free(s_history[0]);
        memmove(&s_history[0], &s_history[1], (MAX_HISTORY - 1) * sizeof(char *));
        s_history[MAX_HISTORY - 1] = entry;
    }
}

void mini_console_history_free(void) {
    for (int i = 0; i < s_history_count; i++) { free(s_history[i]); s_history[i] = NULL; }
    s_history_count = 0;
}

#if defined(ESP_PLATFORM) || defined(ARDUINO)
char *mini_console_readline(MiniVM *vm, const char *prompt, char *line_buf, size_t max_len) {
    (void)vm;
    if (prompt) {
        printf("%s", prompt);
        fflush(stdout);
    }
    return fgets(line_buf, (int)max_len, stdin);
}
#else
char *mini_console_readline(MiniVM *vm, const char *prompt, char *line_buf, size_t max_len) {
    if (!prompt) prompt = "";
    if (!IS_TTY()) {
        printf("%s", prompt);
        fflush(stdout);
        return fgets(line_buf, (int)max_len, stdin);
    }

    printf("%s", prompt);
    fflush(stdout);

    size_t len = 0;
    line_buf[0] = '\0';
    int history_pos = s_history_count;
    char saved_input[2048] = {0};

#if !defined(_WIN32) && !defined(_WIN64)
    struct termios old_t, raw_t;
    tcgetattr(0, &old_t);
    raw_t = old_t;
    raw_t.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(0, TCSANOW, &raw_t);
#endif

    while (1) {
        int c = 0, ext = 0;
#if defined(_WIN32) || defined(_WIN64)
        c = _getch();
        if (c == 0 || c == 224) ext = _getch();
#else
        char ch_buf = 0;
        if (read(0, &ch_buf, 1) <= 0) c = 4;
        else c = (unsigned char)ch_buf;
        if (c == 27) {
            char seq[2];
            if (read(0, &seq[0], 1) > 0 && read(0, &seq[1], 1) > 0) {
                if (seq[0] == '[' || seq[0] == 'O') {
                    if (seq[1] == 'A') ext = 72;
                    else if (seq[1] == 'B') ext = 80;
                    else ext = -1;
                }
            }
        }
#endif

        if (ext == 72 || ext == 80) {
            if (ext == 72 && s_history_count > 0 && history_pos > 0) {
                if (history_pos == s_history_count) {
                    strncpy(saved_input, line_buf, sizeof(saved_input) - 1);
                    saved_input[sizeof(saved_input) - 1] = '\0';
                }
                history_pos--;
            } else if (ext == 80 && history_pos < s_history_count) {
                history_pos++;
            } else {
                continue;
            }
            const char *src = (history_pos == s_history_count) ? saved_input : s_history[history_pos];
            size_t new_len = strlen(src);
            if (new_len < max_len - 2) {
                size_t old_len = len;
                memcpy(line_buf, src, new_len);
                line_buf[new_len] = '\0';
                len = new_len;
                printf("\r%s%s", prompt, line_buf);
                for (size_t i = new_len; i < old_len; i++) putchar(' ');
                for (size_t i = new_len; i < old_len; i++) putchar('\b');
                fflush(stdout);
            }
            continue;
        } else if (ext != 0) {
            continue;
        }

        if (c == '\r' || c == '\n') {
            putchar('\n');
            line_buf[len++] = '\n';
            line_buf[len] = '\0';
            break;
        } else if (c == 3) { /* Ctrl+C */
            printf("^C\n");
            line_buf[0] = '\0';
            break;
        } else if (c == 4) { /* Ctrl+D */
            if (len == 0) {
#if !defined(_WIN32) && !defined(_WIN64)
                tcsetattr(0, TCSANOW, &old_t);
#endif
                return NULL;
            }
        } else if (c == '\b' || c == 127) { /* Backspace */
            if (len > 0) {
                len--;
                line_buf[len] = '\0';
                printf("\b \b");
                fflush(stdout);
            }
        } else if (c == '\t') { /* Tab autocompletion */
            size_t word_start = len;
            while (word_start > 0 && !isspace((unsigned char)line_buf[word_start - 1]) &&
                   line_buf[word_start - 1] != '(' && line_buf[word_start - 1] != ')' &&
                   line_buf[word_start - 1] != '[' && line_buf[word_start - 1] != ']' &&
                   line_buf[word_start - 1] != '\'' && line_buf[word_start - 1] != '"') {
                word_start--;
            }
            char prefix[256];
            size_t prefix_len = len - word_start;
            if (prefix_len >= sizeof(prefix)) prefix_len = sizeof(prefix) - 1;
            memcpy(prefix, line_buf + word_start, prefix_len);
            prefix[prefix_len] = '\0';

            char **matches = NULL;
            int count = mini_get_completions(vm, prefix, &matches);
            if (count == 1) {
                const char *match = matches[0];
                size_t match_len = strlen(match);
                size_t add_len = match_len - prefix_len;
                if (len + add_len < max_len - 2) {
                    memcpy(line_buf + len, match + prefix_len, add_len);
                    len += add_len;
                    line_buf[len] = '\0';
                    printf("%s", match + prefix_len);
                    fflush(stdout);
                }
            } else if (count > 1) {
                size_t lcp_len = strlen(matches[0]);
                for (int i = 1; i < count; i++) {
                    size_t j = 0;
                    while (j < lcp_len && matches[i][j] && matches[0][j] == matches[i][j]) j++;
                    lcp_len = j;
                }
                if (lcp_len > prefix_len) {
                    size_t add_len = lcp_len - prefix_len;
                    if (len + add_len < max_len - 2) {
                        memcpy(line_buf + len, matches[0] + prefix_len, add_len);
                        len += add_len;
                        line_buf[len] = '\0';
                        printf("%.*s", (int)add_len, matches[0] + prefix_len);
                        fflush(stdout);
                    }
                } else {
                    printf("\n");
                    int col = 0;
                    for (int i = 0; i < count; i++) {
                        printf("%-20s", matches[i]);
                        if (++col % 4 == 0 || i + 1 == count) printf("\n");
                    }
                    printf("%s%s", prompt, line_buf);
                    fflush(stdout);
                }
            }
            mini_free_completions(matches, count);
        } else if (c >= 32 && c <= 126) {
            if (len < max_len - 2) {
                line_buf[len++] = (char)c;
                line_buf[len] = '\0';
                putchar(c);
                fflush(stdout);
            }
        }
    }

#if !defined(_WIN32) && !defined(_WIN64)
    tcsetattr(0, TCSANOW, &old_t);
#endif
    return line_buf;
}
#endif

static int count_parens(const char *str) {
    int depth = 0;
    bool in_str = false;
    for (size_t i = 0; str[i]; i++) {
        char c = str[i];
        if (c == '"' && (i == 0 || str[i - 1] != '\\')) {
            in_str = !in_str;
        } else if (!in_str) {
            if (c == ';') break;
            else if (c == '(' || c == '[') depth++;
            else if (c == ')' || c == ']') depth--;
        }
    }
    return depth;
}

static Value prim_readline(MiniVM *vm, int argc, Value *argv) {
    const char *prompt = (argc >= 1 && mini_is_string(argv[0])) ? mini_string_data(argv[0]) : "";
    char buf[4096];
    char *res = mini_console_readline(vm, prompt, buf, sizeof(buf));
    return res ? mini_make_string(vm, res) : mini_false();
}

static Value prim_history_add(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    if (argc >= 1 && mini_is_string(argv[0])) mini_console_history_add(mini_string_data(argv[0]));
    return mini_void();
}

static Value prim_calculate_paren_depth(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    return (argc >= 1 && mini_is_string(argv[0])) ? mini_make_fixnum(count_parens(mini_string_data(argv[0]))) : mini_make_fixnum(0);
}

static Value prim_only_whitespace_p(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    if (argc < 1 || !mini_is_string(argv[0])) return mini_true();
    for (const char *s = mini_string_data(argv[0]); *s; s++) {
        if (!isspace((unsigned char)*s)) return mini_false();
    }
    return mini_true();
}

static Value prim_eval_safely(MiniVM *vm, int argc, Value *argv) {
    if (argc < 1 || !mini_is_string(argv[0])) return mini_void();
    const char *str = mini_string_data(argv[0]);
    Value res = mini_eval_string(vm, str);
    if (mini_get_error(vm)) {
        printf("Error: %s\n", mini_get_error(vm));
        mini_clear_error(vm);
        return mini_void();
    }
    return res;
}

static Value prim_print_banner(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    (void)argc;
    (void)argv;
    printf("============================================================\n"
           " MiniScheme 1.0 (R7RS Bytecode Engine & Interactive REPL)   \n"
           " Type (exit) or press Ctrl+D / Ctrl+Z to exit.             \n"
           " Press Tab to autocomplete function/macro names.           \n"
           "============================================================\n");
    return mini_void();
}

static Value prim_print_cli_help(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    (void)argc;
    (void)argv;
    printf("MiniScheme - Full R7RS Bytecode Scheme Interpreter & REPL\n"
           "Usage: minischeme [options] [script.scm] [args...]\n\n"
           "Options:\n"
           "  -h, --help       Show this help message\n"
           "  -v, --version    Show version information\n"
           "  -e <code>        Evaluate Scheme expression and exit\n"
           "  -l <file.scm>    Load Scheme file before entering REPL\n"
           "  -m <bytes>       Set maximum heap memory limit in bytes\n\n"
           "If no script or -e is provided, MiniScheme starts in interactive REPL mode.\n");
    return mini_void();
}

static Value prim_print_cli_version(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    (void)argc;
    (void)argv;
    printf("MiniScheme 1.0 (Embedded Bytecode Scheme with R7RS Macros & call/cc)\n"
           "Standard library: Built-in precompiled FASL image (zero cold-boot overhead)\n");
    return mini_void();
}

void mini_register_hardware_lib(MiniVM *vm) {
    if (!vm) return;
    static const MiniPrimEntry funcs[] = {
        /* GPIO */
        { "pin-mode",               prim_pin_mode },
        { "digital-write",          prim_digital_write },
        { "digital-read",           prim_digital_read },
        { "analog-read",            prim_analog_read },
        { "pwm-write",              prim_pwm_write },
        /* Time */
        { "millis",                 prim_millis },
        { "micros",                 prim_micros },
        { "delay-ms",               prim_delay_ms },
        { "delay-us",               prim_delay_us },
        /* WiFi */
        { "wifi-connect",           prim_wifi_connect },
        { "wifi-ap",                prim_wifi_ap },
        { "wifi-connected?",        prim_wifi_connected_p },
        { "wifi-ip",                prim_wifi_ip },
        { "wifi-disconnect",        prim_wifi_disconnect },
        { "wifi-status",            prim_wifi_connected_p },
        { "wifi-ssid",              prim_wifi_ssid },
        { "wifi-rssi",              prim_wifi_rssi },
        { "time-synced?",           prim_time_synced_p },
        /* File System */
        { "fs-write",               prim_fs_write },
        { "fs-read",                prim_fs_read },
        { "fs-exists?",             prim_fs_exists_p },
        { "fs-remove",              prim_fs_remove },
        /* System */
        { "free-heap",              prim_free_heap },
        { "max-alloc-heap",         prim_max_alloc_heap },
        { "min-free-heap",          prim_min_free_heap },
        { "heap-total",             prim_heap_total },
        { "vm-allocated-bytes",     prim_vm_allocated_bytes },
        { "vm-heap-size",           prim_vm_heap_size },
        { "vm-stack-depth",         prim_vm_stack_depth },
        { "vm-frame-depth",         prim_vm_frame_depth },
        { "flash-app-bytes",        prim_flash_app_bytes },
        { "flash-total-bytes",      prim_flash_total_bytes },
        { "flash-app-partition-bytes", prim_flash_app_partition_bytes },
        { "app-rom-capacity",       prim_app_rom_capacity },
        { "chip-info",              prim_chip_info },
        { "restart",                prim_restart },
        { "deep-sleep",             prim_deep_sleep },
        { "chip-model",             prim_chip_model },
        /* Console & REPL */
        { "%readline",              prim_readline },
        { "%history-add",           prim_history_add },
        { "%calculate-paren-depth", prim_calculate_paren_depth },
        { "%only-whitespace?",      prim_only_whitespace_p },
        { "%eval-safely",           prim_eval_safely },
        { "%print-banner",          prim_print_banner },
        { "%print-cli-help",        prim_print_cli_help },
        { "%print-cli-version",     prim_print_cli_version },
        /* Compiler & OS Tools */
#if !defined(ESP_PLATFORM) && !defined(ARDUINO)
        /* host files and processes: the PC REPL only (api/spec/host.py) */
        { "system",                 prim_system },
        { "write-file-string",      prim_write_file_string },
        { "file-size",              prim_file_size },
        { "file-remove",            prim_file_remove },
        { "fasl-to-c",              prim_fasl_to_c },
#endif
    };
    mini_register_prim_table(vm, funcs, sizeof(funcs) / sizeof(funcs[0]));
    mini_register_api_lib(vm);   /* (api-version) and the older API names */
}

/* ========================================================================= */
/* Section 5: Dual REPLs (Serial REPL & Web REPL)                            */
/* ========================================================================= */

#define SERIAL_BUF_SIZE 512
static char s_serial_buf[SERIAL_BUF_SIZE];
static size_t s_serial_len = 0;
static int s_paren_depth = 0;

void mini_repl_serial_init(MiniVM *vm) {
    (void)vm;
    s_serial_len = 0;
    s_paren_depth = 0;
    printf("\n=========================================\n     MiniScheme Embedded Serial REPL     \n=========================================\nminischeme> ");
    fflush(stdout);
}

void mini_repl_serial_poll(MiniVM *vm) {
#if defined(ARDUINO)
    while (Serial.available() > 0) {
        char c = (char)Serial.read();
        if (c == '\r') continue;
        if (c == '\b' || c == 127) {
            if (s_serial_len > 0) { s_serial_len--; Serial.print("\b \b"); }
            continue;
        }
        Serial.print(c);
        if (c == '\n') {
            s_serial_buf[s_serial_len] = '\0';
            s_paren_depth += count_parens(s_serial_buf);
            if (s_paren_depth <= 0 && s_serial_len > 0) {
                Value res = mini_eval_string(vm, s_serial_buf);
                if (mini_get_error(vm)) {
                    Serial.printf("Error: %s\n", mini_get_error(vm));
                    mini_clear_error(vm);
                } else if (!mini_is_void(res)) {
                    Serial.print("=> ");
                }
                s_serial_len = 0; s_paren_depth = 0;
                Serial.print("minischeme> ");
            } else if (s_paren_depth > 0) {
                if (s_serial_len < SERIAL_BUF_SIZE - 2) s_serial_buf[s_serial_len++] = '\n';
                Serial.print("... ");
            } else {
                s_serial_len = 0; s_paren_depth = 0;
                Serial.print("minischeme> ");
            }
        } else {
            if (s_serial_len < SERIAL_BUF_SIZE - 2) s_serial_buf[s_serial_len++] = c;
        }
    }
#else
    (void)vm; (void)s_serial_buf; (void)s_serial_len; (void)s_paren_depth; (void)count_parens;
#endif
}

/* Web REPL HTML payload */
static const char WEB_REPL_HTML[] =
    "<!DOCTYPE html><html><head><meta charset='utf-8'><title>MiniScheme Web REPL</title><style>"
    "body{background:#1e1e1e;color:#d4d4d4;font-family:monospace;margin:0;padding:20px;}"
    "#term{background:#252526;border:1px solid #3c3c3c;border-radius:6px;padding:15px;min-height:300px;max-height:600px;overflow-y:auto;}"
    ".out{white-space:pre-wrap;margin:4px 0;}.prompt{color:#569cd6;font-weight:bold;}.err{color:#f44747;}"
    "#inp{width:100%;box-sizing:border-box;background:#1e1e1e;color:#fff;border:1px solid #3c3c3c;border-radius:4px;padding:8px;font-family:monospace;font-size:14px;margin-top:10px;}"
    "</style></head><body><h2>MiniScheme Web REPL</h2><div id='term'><div class='out prompt'>MiniScheme v1.0 Ready. Enter Scheme expression:</div></div>"
    "<textarea id='inp' rows='2' placeholder='(+ 1 2) ... Shift+Enter for newline, Enter to eval'></textarea><script>"
    "const inp=document.getElementById('inp'),term=document.getElementById('term');"
    "inp.addEventListener('keydown',function(e){if(e.key==='Enter'&&!e.shiftKey){e.preventDefault();const code=inp.value.trim();if(!code)return;"
    "term.innerHTML+='<div class=\"out prompt\">&gt; '+code.replace(/</g,'&lt;')+'</div>';inp.value='';term.scrollTop=term.scrollHeight;"
    "fetch('/eval',{method:'POST',body:code}).then(r=>r.text()).then(res=>{term.innerHTML+='<div class=\"out\">'+res.replace(/</g,'&lt;')+'</div>';term.scrollTop=term.scrollHeight;}).catch(err=>{term.innerHTML+='<div class=\"out err\">'+err+'</div>';});}});"
    "</script></body></html>";

void mini_repl_web_init(MiniVM *vm, int port) {
    (void)vm;
    (void)port;
    (void)WEB_REPL_HTML;
}

void mini_repl_web_poll(MiniVM *vm) {
    (void)vm;
}
