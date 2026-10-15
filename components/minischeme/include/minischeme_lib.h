/**
 * @file minischeme_lib.h
 * @brief MiniScheme Platform Library: Hardware Peripherals, File Systems,
 *        WiFi, Dual REPLs, and PC Mock Testing Layer.
 *
 * Supported environments:
 *   1. Arduino IDE (ESP32 / ESP32-S2 / ESP32-S3 / ESP32-C3)
 *   2. ESP-IDF native C environment
 *   3. Desktop PC (Windows / Linux / macOS) with 100% Mock Hardware Simulation
 */

#ifndef MINISCHEME_LIB_H
#define MINISCHEME_LIB_H

#include "minischeme.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================= */
/* Core Library Registration                                                 */
/* ========================================================================= */

/**
 * @brief Register all hardware, time, wifi, filesystem, and system primitives into VM.
 * @param vm MiniScheme VM instance.
 */
void mini_register_hardware_lib(MiniVM *vm);
/* Size of the approm partition, for (app-rom-capacity); 0: none (the app runs from RAM) */
void mini_lib_set_app_rom_capacity(size_t bytes);

/* ========================================================================= */
/* Serial & Web REPL Interfaces                                              */
/* ========================================================================= */

/**
 * @brief Initialize the Serial / Console REPL with multiline parentheses balancing.
 * @param vm MiniScheme VM instance.
 */
void mini_repl_serial_init(MiniVM *vm);

/**
 * @brief Non-blocking poll tick for Serial REPL. Call inside Arduino loop() or IDF task.
 * @param vm MiniScheme VM instance.
 */
void mini_repl_serial_poll(MiniVM *vm);

/**
 * @brief Initialize the embedded Web REPL HTTP server.
 * Serves an interactive HTML/JS terminal and an HTTP /eval API endpoint.
 * @param vm MiniScheme VM instance.
 * @param port HTTP port (default: 80).
 */
void mini_repl_web_init(MiniVM *vm, int port);

/**
 * @brief Non-blocking poll tick for Web REPL server.
 * @param vm MiniScheme VM instance.
 */
void mini_repl_web_poll(MiniVM *vm);

/* ========================================================================= */
/* Console & Interactive Line Reader Library                                 */
/* ========================================================================= */

char *mini_console_readline(MiniVM *vm, const char *prompt, char *line_buf, size_t max_len);
void  mini_console_history_add(const char *cmd);
void  mini_console_history_free(void);

#if defined(ESP_PLATFORM)
void  esp32_wifi_early_init(void);
void  esp32_sntp_init(void);
bool  hal_wifi_connect(const char *ssid, const char *pass);
/* Access point next to the station (APSTA): open when pass is shorter than 8 */
bool  hal_wifi_ap(const char *ssid, const char *pass);
/* Stops reconnecting a station that is not connected (its retries hop the
 * channel and drop the access point's clients); hal_wifi_connect resumes */
void  esp32_wifi_pause_station(void);
/* The station has joined and has an address */
bool  esp32_wifi_connected(void);
bool  hal_time_synced(void);
/* Size and use of the file system behind /spiffs, for (chip-info) */
void  esp32_set_storage_info(bool (*info)(size_t *total, size_t *used));
#endif

/* ========================================================================= */
/* Mock Verification APIs (For PC Unit Testing & Regression Test)           */
/* ========================================================================= */

#if !defined(ARDUINO) && !defined(ESP_PLATFORM)
#define MINISCHEME_MOCK 1

/* Pin mode constants matching Arduino */
#define MINI_PIN_INPUT        0
#define MINI_PIN_OUTPUT       1
#define MINI_PIN_INPUT_PULLUP 2

/* Query & manipulate virtual GPIO pins */
int  mini_mock_get_pin_mode(int pin);
int  mini_mock_get_pin_level(int pin);
void mini_mock_set_pin_level(int pin, int level);

/* Query & manipulate virtual Analog / ADC pins */
int  mini_mock_get_pin_analog(int pin);
void mini_mock_set_pin_analog(int pin, int val);

/* Query PWM output */
int  mini_mock_get_pin_pwm_duty(int pin);
int  mini_mock_get_pin_pwm_freq(int pin);

/* Query & manipulate virtual WiFi state */
bool mini_mock_get_wifi_connected(void);
void mini_mock_set_wifi_connected(bool connected, const char *ip);
const char *mini_mock_get_wifi_ip(void);
const char *mini_mock_get_wifi_ssid(void);

/* Virtual In-Memory File System Sandboxing */
void mini_mock_reset_fs(void);
void mini_mock_add_fs_file(const char *path, const char *content);
const char *mini_mock_get_fs_file(const char *path);
bool mini_mock_fs_exists(const char *path);

/* Virtual Time advancement */
void mini_mock_set_millis(uint32_t ms);
void mini_mock_add_millis(uint32_t ms);

/* Global Mock reset */
void mini_mock_reset_all(void);

#endif /* MINISCHEME_MOCK */

#ifdef __cplusplus
}
#endif

#endif /* MINISCHEME_LIB_H */
