/**
 * @file minischeme_ili9488.h
 * @brief Native ESP32 Driver for ILI9488 8-Bit Parallel 480x320 LCD and Peripherals.
 *
 * Tailored for the ESP32 Desktop Information Card (Open-Source Hardware Desktop Display):
 * - Main MCU: ESP32-WROOM-32 (240MHz Dual-Core)
 * - Display: ILI9488 480x320 (Landscape), 8080 8-bit parallel bus
 *   CS=33, DC=15, RST=32, WR=4, RD=2
 *   D0..D7 = 12, 13, 26, 25, 17, 16, 27, 14
 * - Backlight: GPIO 22 (LEDC PWM 5000Hz 8-bit)
 * - WS2812 RGB LED: GPIO 21
 * - Photoresistor: GPIO 35 (ADC1_CHANNEL_7)
 * - Tactile Button: GPIO 18 (Active-high with 10k pulldown)
 */

#ifndef MINISCHEME_ILI9488_H
#define MINISCHEME_ILI9488_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "minischeme_gui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Hardware Pinout */
#define ESP32_TFT_CS    33
#define ESP32_TFT_DC    15
#define ESP32_TFT_RST   32
#define ESP32_TFT_WR    4
#define ESP32_TFT_RD    2

#define ESP32_TFT_D0    12
#define ESP32_TFT_D1    13
#define ESP32_TFT_D2    26
#define ESP32_TFT_D3    25
#define ESP32_TFT_D4    17
#define ESP32_TFT_D5    16
#define ESP32_TFT_D6    27
#define ESP32_TFT_D7    14

#define ESP32_PIN_BL    22
#define ESP32_PIN_WS2812 21
#define ESP32_PIN_LIGHT 35
#define ESP32_PIN_BTN   18

/* Display Resolutions (Default: 320x480 Portrait) */
#define ESP32_TFT_WIDTH  320
#define ESP32_TFT_HEIGHT 480

/* Driver Initialization, Rotation & Flushing */
void esp32_ili9488_init(void);
void esp32_ili9488_set_rotation(uint8_t m);
/* Display inversion: on for an IPS panel (the default), off for a TFT one; set
 * before esp32_ili9488_init, or later (sent at once) */
void esp32_ili9488_set_inversion(bool on);
bool esp32_ili9488_get_inversion(void);
/* The panel controller read at init (0xD3): "ILI9488", "ST7796" or "unknown" */
const char *esp32_ili9488_controller(void);
uint8_t esp32_ili9488_get_rotation(void);
int  esp32_ili9488_get_width(void);
int  esp32_ili9488_get_height(void);
void esp32_ili9488_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);
void esp32_ili9488_flush(const uint16_t *vfb, int width, int height);
void esp32_ili9488_flush_rect(const uint16_t *vfb, int stride, int x, int y, int w, int h);
void esp32_ili9488_flush_dirty(const uint16_t *vfb, int stride);
void esp32_ili9488_set_pixel_format(uint8_t fmt);
void esp32_ili9488_flush_rect_rgb666(const uint32_t *pixels, int stride, int x, int y, int w, int h);

/* RGB565 band transport: I2S DMA after start_dma(), GPIO bit-bang otherwise.
 * pixels holds h full-width rows whose first row is screen row y. */
/* max_transfer_bytes: one DMA chunk (whole rows). The self-test always runs
 * in RGB565; rgb666 selects the byte order used afterwards. */
bool esp32_ili9488_start_dma(size_t max_transfer_bytes, uint32_t pclk_hz, bool rgb666);
/* Panel bus format: RGB666 (3 bytes/pixel) or RGB565 (2 bytes/pixel) */
void esp32_ili9488_set_bus_rgb666(bool on);
bool esp32_ili9488_bus_rgb666(void);
bool esp32_ili9488_dma_active(void);
/* Bytes held by the I2S driver's DMA format buffer (0 when DMA is off) */
size_t esp32_ili9488_dma_ram_bytes(void);
void esp32_ili9488_flush_band(const uint16_t *pixels, int width, int y, int h);
/* Allocates the staging buffer used when rows need converting on the way out
 * (RGB666 bus or palette-indexed rows); returns its size (0 without DMA) */
size_t esp32_ili9488_reserve_staging(void);
/* Full-width rows of any color bits (MiniGuiRows), first row = screen row y */
void esp32_ili9488_flush_rows(const MiniGuiRows *rows, int width, int y, int h);
/* Rows `width` pixels wide (stride = their bytes) for screen columns
 * [x0, x0 + width), rows [y, y + h): one window, one transfer */
void esp32_ili9488_flush_rows_at(const MiniGuiRows *rows, int x0, int y, int width, int h);
/* Solid rectangle, the DIRECT-mode primitive */
void esp32_ili9488_fill(int x, int y, int w, int h, uint16_t rgb565);

/* Double-Buffered Asynchronous DMA Band Transfer */
#define ESP32_DMA_BAND_HEIGHT 16
#define ESP32_DMA_BUF_PIXELS  (ESP32_TFT_WIDTH * ESP32_DMA_BAND_HEIGHT)

void esp32_ili9488_dma_init(void);
void esp32_ili9488_dma_wait(void);
void esp32_ili9488_flush_band_dma(int band_index, const uint16_t *pixels, int count);

/* Backlight PWM Control (0..255) */
void esp32_backlight_init(void);
void esp32_backlight_set(uint8_t brightness);

/* WS2812 RGB LED (IO21) */
void esp32_ws2812_init(void);
void esp32_ws2812_set(uint8_t r, uint8_t g, uint8_t b);

/* Photoresistor Light Sensor (IO35 / ADC1_CH7) */
void esp32_light_init(void);
int  esp32_light_read(void);
/* User pins: ADC1 (GPIO 32..39; -1 for other pins) and LEDC PWM on its own
 * timer (the backlight keeps timer 0 and channel 0) */
int  esp32_adc_read(int pin);
bool esp32_pwm_write(int pin, int freq_hz, int duty);

/* User Tactile Button (IO18) */
void esp32_button_init(void);
bool esp32_button_read(void);
/* Presses counted by the button interrupt (the main loop sees short presses
 * that began and ended within one of its passes) */
uint32_t esp32_button_presses(void);

/* Synchronize hardware inputs and outputs with MiniScheme GUI state */
void esp32_hardware_poll_and_sync(MiniVM *vm);

#ifdef __cplusplus
}
#endif

#endif /* MINISCHEME_ILI9488_H */
