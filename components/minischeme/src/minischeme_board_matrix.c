/**
 * @file minischeme_board_matrix.c
 * @brief Board support for the Waveshare ESP32-S3-Matrix (ESP32-S3FH4R2: 4 MB flash, 2 MB
 *        PSRAM, research build MINI_BOARD=matrix).
 *
 * The same interface as minischeme_ili9488.c, for a board with:
 * - No display: the GUI draws to a virtual 172 x 320 screen that is never sent anywhere; the
 *   WebREPL mirror and screenshots still show it
 * - 8 x 8 RGB LED matrix (WS2812, data on GPIO 14), sent with the RMT peripheral
 *   (minischeme_matrix.h); the single status LED of the other boards is the whole matrix here
 * - Button: BOOT (GPIO 0, low when pressed)
 * - No light sensor, no backlight (the QMI8658 motion sensor is not used yet)
 */

#include "minischeme_ili9488.h"
#include "minischeme_matrix.h"
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define MX_PIN_LEDS   14
#define MX_PIN_BTN    0
#define MX_LEDS       (MINI_MATRIX_W * MINI_MATRIX_H)

#define PANEL_W       172         /* the virtual screen: the size of the other S3 board */
#define PANEL_H       320

static const char *TAG = "board_matrix";
static uint8_t s_rotation = 0;
static bool s_invert = false;

/* ------------------------------------------------------------------------- */
/* The display that is not there                                             */
/* ------------------------------------------------------------------------- */

void esp32_ili9488_set_inversion(bool on) { s_invert = on; }
bool esp32_ili9488_get_inversion(void) { return s_invert; }
const char *esp32_ili9488_controller(void) { return "none"; }
void esp32_ili9488_set_tuning(int gamma_mode, const uint8_t *pos, size_t npos, const uint8_t *neg, size_t nneg,
                              const uint8_t *vcom, size_t nvcom) {
    (void)gamma_mode; (void)pos; (void)npos; (void)neg; (void)nneg; (void)vcom; (void)nvcom;
}
void esp32_ili9488_set_rotation(uint8_t m) { s_rotation = m & 3; }
uint8_t esp32_ili9488_get_rotation(void) { return s_rotation; }
int esp32_ili9488_get_width(void) { return (s_rotation & 1) ? PANEL_H : PANEL_W; }
int esp32_ili9488_get_height(void) { return (s_rotation & 1) ? PANEL_W : PANEL_H; }
void esp32_ili9488_init(void) { ESP_LOGI(TAG, "no display: virtual %dx%d screen", PANEL_W, PANEL_H); }
void esp32_ili9488_set_pixel_format(uint8_t fmt) { (void)fmt; }
bool esp32_ili9488_start_dma(size_t max_transfer_bytes, uint32_t pclk_hz, bool rgb666) {
    (void)max_transfer_bytes; (void)pclk_hz; (void)rgb666;
    return true;
}
void esp32_ili9488_set_bus_rgb666(bool on) { (void)on; }
bool esp32_ili9488_bus_rgb666(void) { return false; }
bool esp32_ili9488_dma_active(void) { return true; }
size_t esp32_ili9488_dma_ram_bytes(void) { return 0; }
size_t esp32_ili9488_reserve_staging(void) { return 0; }
void esp32_ili9488_flush_rows_at(const MiniGuiRows *rows, int x0, int y, int width, int h) {
    (void)rows; (void)x0; (void)y; (void)width; (void)h;
}
void esp32_ili9488_flush_rows(const MiniGuiRows *rows, int width, int y, int h) {
    (void)rows; (void)width; (void)y; (void)h;
}
void esp32_ili9488_flush_band(const uint16_t *pixels, int width, int y, int h) {
    (void)pixels; (void)width; (void)y; (void)h;
}
void esp32_ili9488_fill(int x, int y, int w, int h, uint16_t rgb565) {
    (void)x; (void)y; (void)w; (void)h; (void)rgb565;
}
void esp32_ili9488_flush_rect(const uint16_t *vfb, int stride, int x, int y, int w, int h) {
    (void)vfb; (void)stride; (void)x; (void)y; (void)w; (void)h;
}
void esp32_ili9488_flush(const uint16_t *vfb, int width, int height) { (void)vfb; (void)width; (void)height; }
void esp32_ili9488_flush_dirty(const uint16_t *vfb, int stride) {
    (void)vfb; (void)stride;
    mini_gui_clear_dirty();
}
void esp32_ili9488_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    (void)x0; (void)y0; (void)x1; (void)y1;
}
void esp32_ili9488_flush_rect_rgb666(const uint32_t *pixels, int stride, int x, int y, int w, int h) {
    (void)pixels; (void)stride; (void)x; (void)y; (void)w; (void)h;
}
void esp32_ili9488_dma_init(void) {}
void esp32_ili9488_dma_wait(void) {}
void esp32_ili9488_flush_band_dma(int band_index, const uint16_t *pixels, int count) {
    (void)band_index; (void)pixels; (void)count;
}

void esp32_backlight_init(void) {}
void esp32_backlight_set(uint8_t brightness) { (void)brightness; }
void esp32_light_init(void) {}
int esp32_light_read(void) { return 2048; }   /* no sensor: the middle of the range */
int esp32_adc_read(int pin) { (void)pin; return -1; }

/* PWM for apps: LEDC timer 1, channels 1..7, one per pin; 8-bit duty */
bool esp32_pwm_write(int pin, int freq_hz, int duty) {
    static int8_t chan_pin[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
    static int timer_hz = 0;
    if (pin < 0 || pin > 48 || pin == MX_PIN_LEDS) return false;
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

/* ------------------------------------------------------------------------- */
/* The BOOT button: low when pressed. An edge interrupt counts presses.      */
/* ------------------------------------------------------------------------- */

static volatile uint32_t s_btn_presses = 0;
static volatile int64_t s_btn_up_since = 0;

static void IRAM_ATTR button_isr(void *arg) {
    (void)arg;
    int64_t now = esp_timer_get_time();
    if (gpio_get_level(MX_PIN_BTN) == 0) {
        if (now - s_btn_up_since > 30000) s_btn_presses++;
    } else {
        s_btn_up_since = now;
    }
}

void esp32_button_init(void) {
    gpio_config_t btn_conf = {
        .pin_bit_mask = (1ULL << MX_PIN_BTN), .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    gpio_config(&btn_conf);
    esp_err_t err = gpio_install_isr_service(0);
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) gpio_isr_handler_add(MX_PIN_BTN, button_isr, NULL);
}

uint32_t esp32_button_presses(void) { return s_btn_presses; }
bool esp32_button_read(void) { return gpio_get_level(MX_PIN_BTN) == 0; }

/* ------------------------------------------------------------------------- */
/* The LED matrix                                                            */
/* ------------------------------------------------------------------------- */

static rmt_channel_handle_t s_chan;
static rmt_encoder_handle_t s_enc;
static uint32_t s_pixels[MX_LEDS];          /* 0xRRGGBB, row by row from the top left */
static uint8_t s_wire[MX_LEDS * 3];         /* as sent */
static int s_brightness = 25;               /* percent of MINI_MATRIX_MAX_LEVEL */
static bool s_rgb_order = false;
static bool s_serpentine = false;
static int s_rotate = 0;
static int64_t s_last_show_us = 0;

bool mini_matrix_init(void) {
    if (s_chan) return true;
    /* 10 MHz ticks: a 0 is 0.4 us high + 0.8 us low, a 1 is 0.8 us high + 0.4 us low */
    rmt_bytes_encoder_config_t enc_cfg = {
        .bit0 = { .level0 = 1, .duration0 = 4, .level1 = 0, .duration1 = 8 },
        .bit1 = { .level0 = 1, .duration0 = 8, .level1 = 0, .duration1 = 4 },
        .flags.msb_first = 1,
    };
    rmt_tx_channel_config_t cfg = {
        .gpio_num = MX_PIN_LEDS, .clk_src = RMT_CLK_SRC_DEFAULT, .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 1024, .trans_queue_depth = 1, .flags.with_dma = 1,
    };
    if (rmt_new_tx_channel(&cfg, &s_chan) != ESP_OK) {
        /* without DMA: the interrupt refills the channel's 48 symbols as they go out */
        cfg.flags.with_dma = 0;
        cfg.mem_block_symbols = 96;
        s_chan = NULL;
        if (rmt_new_tx_channel(&cfg, &s_chan) != ESP_OK) {
            ESP_LOGE(TAG, "the LED matrix output could not be set up");
            s_chan = NULL;
            return false;
        }
    }
    if (rmt_new_bytes_encoder(&enc_cfg, &s_enc) != ESP_OK || rmt_enable(s_chan) != ESP_OK) {
        ESP_LOGE(TAG, "the LED matrix encoder could not be set up");
        rmt_del_channel(s_chan);
        s_chan = NULL;
        return false;
    }
    memset(s_pixels, 0, sizeof(s_pixels));
    mini_matrix_show();
    ESP_LOGI(TAG, "LED matrix %dx%d on GPIO %d", MINI_MATRIX_W, MINI_MATRIX_H, MX_PIN_LEDS);
    return true;
}

bool mini_matrix_ready(void) { return s_chan != NULL; }

void mini_matrix_configure(bool rgb_order, bool serpentine, int rotate) {
    s_rgb_order = rgb_order;
    s_serpentine = serpentine;
    s_rotate = rotate & 3;
}

void mini_matrix_set(int x, int y, uint32_t rgb) {
    if (x < 0 || y < 0 || x >= MINI_MATRIX_W || y >= MINI_MATRIX_H) return;
    s_pixels[y * MINI_MATRIX_W + x] = rgb & 0xffffffu;
}

uint32_t mini_matrix_get(int x, int y) {
    if (x < 0 || y < 0 || x >= MINI_MATRIX_W || y >= MINI_MATRIX_H) return 0;
    return s_pixels[y * MINI_MATRIX_W + x];
}

void mini_matrix_fill(uint32_t rgb) {
    for (int i = 0; i < MX_LEDS; i++) s_pixels[i] = rgb & 0xffffffu;
}

void mini_matrix_set_brightness(int percent) {
    s_brightness = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
}

int mini_matrix_get_brightness(void) { return s_brightness; }

/* The LED (its number on the data line) that shows pixel (x, y) */
static int led_of(int x, int y) {
    const int n = MINI_MATRIX_W - 1;         /* the matrix is square */
    int px = x, py = y;
    switch (s_rotate) {
        case 1: px = y;     py = n - x; break;
        case 2: px = n - x; py = n - y; break;
        case 3: px = n - y; py = x;     break;
        default: break;
    }
    if (s_serpentine && (py & 1)) px = n - px;
    return py * MINI_MATRIX_W + px;
}

bool mini_matrix_show(void) {
    if (!s_chan) return false;
    /* a channel value v is sent as v * level / 255, level at most MINI_MATRIX_MAX_LEVEL */
    uint32_t level = (uint32_t)s_brightness * MINI_MATRIX_MAX_LEVEL / 100;
    for (int y = 0; y < MINI_MATRIX_H; y++) {
        for (int x = 0; x < MINI_MATRIX_W; x++) {
            uint32_t c = s_pixels[y * MINI_MATRIX_W + x];
            uint8_t r = (uint8_t)(((c >> 16) & 0xff) * level / 255);
            uint8_t g = (uint8_t)(((c >> 8) & 0xff) * level / 255);
            uint8_t b = (uint8_t)((c & 0xff) * level / 255);
            /* a colour that is not black stays visible at low brightness */
            if (level > 0) {
                if (!r && ((c >> 16) & 0xff)) r = 1;
                if (!g && ((c >> 8) & 0xff)) g = 1;
                if (!b && (c & 0xff)) b = 1;
            }
            uint8_t *w = &s_wire[led_of(x, y) * 3];
            w[0] = s_rgb_order ? r : g;
            w[1] = s_rgb_order ? g : r;
            w[2] = b;
        }
    }
    /* the LEDs latch after the line has been low for 0.3 ms */
    int64_t since = esp_timer_get_time() - s_last_show_us;
    if (since >= 0 && since < 400) esp_rom_delay_us((uint32_t)(400 - since));
    rmt_transmit_config_t tx = { .loop_count = 0 };
    bool ok = rmt_transmit(s_chan, s_enc, s_wire, sizeof(s_wire), &tx) == ESP_OK &&
              rmt_tx_wait_all_done(s_chan, 100) == ESP_OK;
    s_last_show_us = esp_timer_get_time();
    return ok;
}

/* The status LED of the other boards: here every LED of the matrix */
void esp32_ws2812_init(void) { mini_matrix_init(); }

void esp32_ws2812_set(uint8_t r, uint8_t g, uint8_t b) {
    mini_matrix_fill(((uint32_t)r << 16) | ((uint32_t)g << 8) | b);
    mini_matrix_show();
}

void esp32_hardware_poll_and_sync(MiniVM *vm) {
    (void)vm;
    mini_button_set(esp32_button_read());
    mini_light_set(esp32_light_read());
    if (mini_gui_is_dirty()) mini_gui_clear_dirty();
}
