/**
 * @file minischeme_board_s3.c
 * @brief Board support for the Waveshare ESP32-S3-LCD-1.47 (ESP32-S3R8, research build).
 *
 * The same interface as minischeme_ili9488.c (the ESP32 desk display), for a board with:
 * - Display: ST7789, 172 x 320, SPI (MOSI 45, SCLK 40, CS 42, DC 41, RST 39), backlight 48
 * - RGB LED (WS2812): GPIO 38
 * - Button: BOOT (GPIO 0, low when pressed)
 * - No light sensor
 * Rows wider or taller than the panel are cut at its right and bottom edges.
 */

#include "minischeme_ili9488.h"
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/gpio_struct.h"

#define S3_LCD_MOSI   45
#define S3_LCD_SCLK   40
#define S3_LCD_CS     42
#define S3_LCD_DC     41
#define S3_LCD_RST    39
#define S3_PIN_BL     48
#define S3_PIN_WS2812 38
#define S3_PIN_BTN    0

#define PANEL_W       172
#define PANEL_H       320
#define PANEL_X_GAP   34          /* the 172 columns sit in the middle of the controller's 240 */
#define LCD_HZ        (40 * 1000 * 1000)
#define CHUNK_ROWS    16
#define CHUNK_PIXELS  (PANEL_H * CHUNK_ROWS)   /* a chunk of rows in either orientation */

static const char *TAG = "board_s3";
static spi_device_handle_t s_spi;
static uint16_t *s_chunk;          /* big-endian pixels on their way out (DMA capable) */
static bool s_invert = true;
static uint8_t s_rotation = 0;

static void lcd_send(bool data, const void *bytes, size_t n) {
    if (!s_spi || n == 0) return;
    spi_transaction_t t = { 0 };
    t.length = n * 8;
    t.tx_buffer = bytes;
    gpio_set_level(S3_LCD_DC, data ? 1 : 0);
    spi_device_polling_transmit(s_spi, &t);
}

static void lcd_cmd(uint8_t cmd, const uint8_t *args, size_t n) {
    lcd_send(false, &cmd, 1);
    if (n) {
        uint8_t copy[16];
        if (n > sizeof(copy)) n = sizeof(copy);
        memcpy(copy, args, n);
        lcd_send(true, copy, n);
    }
}

static int panel_w(void) { return (s_rotation & 1) ? PANEL_H : PANEL_W; }
static int panel_h(void) { return (s_rotation & 1) ? PANEL_W : PANEL_H; }

static void lcd_window(int x, int y, int w, int h) {
    int xg = (s_rotation & 1) ? 0 : PANEL_X_GAP;
    int yg = (s_rotation & 1) ? PANEL_X_GAP : 0;
    int x0 = x + xg, x1 = x + w - 1 + xg, y0 = y + yg, y1 = y + h - 1 + yg;
    uint8_t col[4] = { (uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1 };
    uint8_t row[4] = { (uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)(y1 >> 8), (uint8_t)y1 };
    lcd_cmd(0x2A, col, 4);
    lcd_cmd(0x2B, row, 4);
    lcd_cmd(0x2C, NULL, 0);
}

void esp32_ili9488_set_inversion(bool on) {
    s_invert = on;
    if (s_spi) lcd_cmd(on ? 0x21 : 0x20, NULL, 0);
}
bool esp32_ili9488_get_inversion(void) { return s_invert; }
const char *esp32_ili9488_controller(void) { return "ST7789"; }
void esp32_ili9488_set_tuning(int gamma_mode, const uint8_t *pos, size_t npos, const uint8_t *neg, size_t nneg,
                              const uint8_t *vcom, size_t nvcom) {
    (void)gamma_mode; (void)pos; (void)npos; (void)neg; (void)nneg; (void)vcom; (void)nvcom;
}

void esp32_ili9488_set_rotation(uint8_t m) {
    static const uint8_t madctl[4] = { 0x00, 0x60, 0xC0, 0xA0 };
    s_rotation = m & 3;
    if (s_spi) lcd_cmd(0x36, &madctl[s_rotation], 1);
}
uint8_t esp32_ili9488_get_rotation(void) { return s_rotation; }
int esp32_ili9488_get_width(void) { return panel_w(); }
int esp32_ili9488_get_height(void) { return panel_h(); }

void esp32_ili9488_init(void) {
    static const uint8_t rgb565_format = 0x55;
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << S3_LCD_DC) | (1ULL << S3_LCD_RST), .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out);
    spi_bus_config_t bus = {
        .mosi_io_num = S3_LCD_MOSI, .miso_io_num = -1, .sclk_io_num = S3_LCD_SCLK,
        .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = CHUNK_PIXELS * 2,
    };
    spi_device_interface_config_t dev = {
        .clock_speed_hz = LCD_HZ, .mode = 0, .spics_io_num = S3_LCD_CS, .queue_size = 1,
    };
    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK ||
        spi_bus_add_device(SPI2_HOST, &dev, &s_spi) != ESP_OK) {
        ESP_LOGE(TAG, "the display's SPI bus could not be set up");
        s_spi = NULL;
        return;
    }
    s_chunk = (uint16_t *)heap_caps_malloc(CHUNK_PIXELS * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    gpio_set_level(S3_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(S3_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
    lcd_cmd(0x11, NULL, 0);                      /* sleep out */
    vTaskDelay(pdMS_TO_TICKS(120));
    lcd_cmd(0x3A, &rgb565_format, 1);
    esp32_ili9488_set_rotation(s_rotation);
    lcd_cmd(s_invert ? 0x21 : 0x20, NULL, 0);
    lcd_cmd(0x13, NULL, 0);                      /* normal display */
    esp32_ili9488_fill(0, 0, panel_w(), panel_h(), 0x0000);
    lcd_cmd(0x29, NULL, 0);                      /* display on */
    ESP_LOGI(TAG, "ST7789 %dx%d on SPI at %d MHz", PANEL_W, PANEL_H, LCD_HZ / 1000000);
}

void esp32_ili9488_set_pixel_format(uint8_t fmt) { (void)fmt; }
bool esp32_ili9488_start_dma(size_t max_transfer_bytes, uint32_t pclk_hz, bool rgb666) {
    (void)max_transfer_bytes; (void)pclk_hz; (void)rgb666;
    return true;   /* SPI with DMA from the start */
}
void esp32_ili9488_set_bus_rgb666(bool on) { (void)on; }
bool esp32_ili9488_bus_rgb666(void) { return false; }
bool esp32_ili9488_dma_active(void) { return s_spi != NULL; }
size_t esp32_ili9488_dma_ram_bytes(void) { return s_chunk ? CHUNK_PIXELS * 2 : 0; }
size_t esp32_ili9488_reserve_staging(void) { return 0; }

void esp32_ili9488_flush_rows_at(const MiniGuiRows *rows, int x0, int y, int width, int h) {
    if (!rows || !rows->data || !s_spi || !s_chunk || width <= 0 || h <= 0) return;
    int pw = panel_w(), ph = panel_h();
    if (x0 < 0 || y < 0 || x0 >= pw || y >= ph) return;
    int w = x0 + width > pw ? pw - x0 : width;
    if (y + h > ph) h = ph - y;
    int per = CHUNK_PIXELS / w;
    lcd_window(x0, y, w, h);
    for (int r = 0; r < h; r += per) {
        int n = r + per > h ? h - r : per;
        for (int i = 0; i < n; i++) {
            uint16_t *line = s_chunk + (size_t)i * (size_t)w;
            mini_gui_rows_to_rgb565(rows, r + i, 0, w, line);
            for (int x = 0; x < w; x++) line[x] = (uint16_t)((line[x] << 8) | (line[x] >> 8));
        }
        lcd_send(true, s_chunk, (size_t)n * (size_t)w * 2);
    }
}

void esp32_ili9488_flush_rows(const MiniGuiRows *rows, int width, int y, int h) {
    esp32_ili9488_flush_rows_at(rows, 0, y, width, h);
}

void esp32_ili9488_flush_band(const uint16_t *pixels, int width, int y, int h) {
    MiniGuiRows rows = { (const uint8_t *)pixels, (size_t)width * 2, 16, NULL };
    esp32_ili9488_flush_rows_at(&rows, 0, y, width, h);
}

void esp32_ili9488_fill(int x, int y, int w, int h, uint16_t rgb565) {
    if (!s_spi || !s_chunk || w <= 0 || h <= 0) return;
    int pw = panel_w(), ph = panel_h();
    if (x < 0 || y < 0 || x >= pw || y >= ph) return;
    if (x + w > pw) w = pw - x;
    if (y + h > ph) h = ph - y;
    size_t total = (size_t)w * (size_t)h;
    size_t first = total < CHUNK_PIXELS ? total : CHUNK_PIXELS;
    uint16_t be = (uint16_t)((rgb565 << 8) | (rgb565 >> 8));
    for (size_t i = 0; i < first; i++) s_chunk[i] = be;
    lcd_window(x, y, w, h);
    while (total) {
        size_t n = total < CHUNK_PIXELS ? total : CHUNK_PIXELS;
        lcd_send(true, s_chunk, n * 2);
        total -= n;
    }
}

void esp32_ili9488_flush_rect(const uint16_t *vfb, int stride, int x, int y, int w, int h) {
    if (!vfb) return;
    MiniGuiRows rows = { (const uint8_t *)(vfb + (size_t)y * (size_t)stride + x), (size_t)stride * 2, 16, NULL };
    esp32_ili9488_flush_rows_at(&rows, x, y, w, h);
}
void esp32_ili9488_flush(const uint16_t *vfb, int width, int height) {
    esp32_ili9488_flush_rect(vfb, width, 0, 0, width, height);
}
void esp32_ili9488_flush_dirty(const uint16_t *vfb, int stride) {
    esp32_ili9488_flush_rect(vfb, stride, 0, 0, mini_gui_get_width(), mini_gui_get_height());
    mini_gui_clear_dirty();
}
void esp32_ili9488_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    lcd_window(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
}
void esp32_ili9488_flush_rect_rgb666(const uint32_t *pixels, int stride, int x, int y, int w, int h) {
    (void)pixels; (void)stride; (void)x; (void)y; (void)w; (void)h;
}
void esp32_ili9488_dma_init(void) {}
void esp32_ili9488_dma_wait(void) {}
void esp32_ili9488_flush_band_dma(int band_index, const uint16_t *pixels, int count) {
    (void)band_index; (void)pixels; (void)count;
}

/* Backlight: LEDC timer 0, channel 0 */
void esp32_backlight_init(void) {
    ledc_timer_config_t timer_conf = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0, .freq_hz = 5000, .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer_conf);
    ledc_channel_config_t ch_conf = {
        .gpio_num = S3_PIN_BL, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE, .timer_sel = LEDC_TIMER_0, .duty = 255, .hpoint = 0,
    };
    ledc_channel_config(&ch_conf);
}

void esp32_backlight_set(uint8_t brightness) {
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, brightness);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

void esp32_light_init(void) {}
int esp32_light_read(void) { return 2048; }   /* no sensor: the middle of the range */
int esp32_adc_read(int pin) { (void)pin; return -1; }

/* PWM for apps: LEDC timer 1, channels 1..7, one per pin; 8-bit duty */
bool esp32_pwm_write(int pin, int freq_hz, int duty) {
    static int8_t chan_pin[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };   /* channel 0: the backlight */
    static int timer_hz = 0;
    if (pin < 0 || pin > 48 || pin == S3_PIN_BL) return false;
    if (freq_hz <= 0) freq_hz = 5000;
    duty = duty < 0 ? 0 : (duty > 255 ? 255 : duty);
    if (freq_hz != timer_hz) {
        ledc_timer_config_t t = {
            .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_8_BIT,
            .timer_num = LEDC_TIMER_1, .freq_hz = (uint32_t)freq_hz, .clk_cfg = LEDC_AUTO_CLK,
        };
        if (ledc_timer_config(&t) != ESP_OK) return false;
        timer_hz = freq_hz;
    }
    int ch = -1;
    for (int i = 1; i < 8; i++) {
        if (chan_pin[i] == pin) { ch = i; break; }
        if (ch < 0 && chan_pin[i] < 0) ch = i;
    }
    if (ch < 0) return false;
    if (chan_pin[ch] != pin) {
        ledc_channel_config_t c = {
            .gpio_num = pin, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = (ledc_channel_t)ch,
            .intr_type = LEDC_INTR_DISABLE, .timer_sel = LEDC_TIMER_1, .duty = (uint32_t)duty, .hpoint = 0,
        };
        if (ledc_channel_config(&c) != ESP_OK) return false;
        chan_pin[ch] = (int8_t)pin;
        return true;
    }
    ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)ch, (uint32_t)duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)ch);
    return true;
}

/* The BOOT button: low when pressed. An edge interrupt counts presses. */
static volatile uint32_t s_btn_presses = 0;
static volatile int64_t s_btn_up_since = 0;

static void IRAM_ATTR button_isr(void *arg) {
    (void)arg;
    int64_t now = esp_timer_get_time();
    if (gpio_get_level(S3_PIN_BTN) == 0) {
        if (now - s_btn_up_since > 30000) s_btn_presses++;
    } else {
        s_btn_up_since = now;
    }
}

void esp32_button_init(void) {
    gpio_config_t btn_conf = {
        .pin_bit_mask = (1ULL << S3_PIN_BTN), .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    gpio_config(&btn_conf);
    esp_err_t err = gpio_install_isr_service(0);
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) gpio_isr_handler_add(S3_PIN_BTN, button_isr, NULL);
}

uint32_t esp32_button_presses(void) { return s_btn_presses; }
bool esp32_button_read(void) { return gpio_get_level(S3_PIN_BTN) == 0; }

/* WS2812 on GPIO 38, timed with the CPU cycle counter (240 MHz) */
void esp32_ws2812_init(void) {
    gpio_set_direction(S3_PIN_WS2812, GPIO_MODE_OUTPUT);
    gpio_set_level(S3_PIN_WS2812, 0);
}

static inline void wait_cycles(uint32_t start, uint32_t cycles) {
    while ((uint32_t)(esp_cpu_get_cycle_count() - start) < cycles) { }
}

void esp32_ws2812_set(uint8_t r, uint8_t g, uint8_t b) {
    uint8_t bytes[3] = { g, r, b };
    const uint32_t mask = 1u << (S3_PIN_WS2812 - 32);
    portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    taskENTER_CRITICAL(&mux);
    for (int i = 0; i < 3; i++) {
        for (int bit = 7; bit >= 0; bit--) {
            uint32_t t0 = esp_cpu_get_cycle_count();
            GPIO.out1_w1ts.val = mask;
            wait_cycles(t0, ((bytes[i] >> bit) & 1) ? 192 : 84);   /* 0.8 us / 0.35 us high */
            GPIO.out1_w1tc.val = mask;
            wait_cycles(t0, 300);                                  /* 1.25 us a bit */
        }
    }
    taskEXIT_CRITICAL(&mux);
}

void esp32_hardware_poll_and_sync(MiniVM *vm) {
    (void)vm;
    mini_button_set(esp32_button_read());
    mini_light_set(esp32_light_read());
    if (mini_gui_is_dirty()) {
        esp32_ili9488_flush_dirty(mini_gui_get_vfb(), mini_gui_get_width());
    }
}
