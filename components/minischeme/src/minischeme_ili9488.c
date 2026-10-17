/**
 * @file minischeme_ili9488.c
 * @brief Native ESP32 Driver for ILI9488 8-Bit Parallel 480x320 LCD and Peripherals.
 *
 * Implements high-throughput parallel bus pixel transfers via ESP32 GPIO W1TS/W1TC registers,
 * ILI9488 init sequence based on canonical F:/esp/ili9488 driver, LEDC backlight PWM,
 * WS2812 LED control, photoresistor ADC, and GPIO 18 button debounce.
 */

#include "minischeme_ili9488.h"
#include <stdio.h>
#include <string.h>

#if defined(ESP_PLATFORM) || defined(ARDUINO)

#include "soc/soc.h"
#include "soc/gpio_reg.h"
#include "soc/gpio_struct.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#if __has_include("esp_adc/adc_oneshot.h")
#include "esp_adc/adc_oneshot.h"
static adc_oneshot_unit_handle_t s_adc_handle = NULL;
#elif __has_include("driver/adc.h")
#include "driver/adc.h"
#endif
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Canonical Pin Definitions from F:/esp/ili9488/ili9488.c */
#define TFT_CS   33  /* Chip select control pin */
#define TFT_DC   15  /* Data Command control pin */
#define TFT_RST  32  /* Reset pin */
#define TFT_WR    4  /* Write strobe control pin */
#define TFT_RD    2  /* Read strobe control pin */

#define TFT_D0   12
#define TFT_D1   13
#define TFT_D2   26
#define TFT_D3   25
#define TFT_D4   17
#define TFT_D5   16
#define TFT_D6   27
#define TFT_D7   14

#define TFT_WIDTH  320
#define TFT_HEIGHT 480

/* ILI9488 Commands from ILI9488_Defines.h */
#define TFT_NOP     0x00
#define TFT_SWRST   0x01
#define TFT_SLPIN   0x10
#define TFT_SLPOUT  0x11
#define TFT_INVOFF  0x20
#define TFT_INVON   0x21
#define TFT_DISPOFF 0x28
#define TFT_DISPON  0x29
#define TFT_CASET   0x2A
#define TFT_PASET   0x2B
#define TFT_RAMWR   0x2C
#define TFT_RAMRD   0x2E
#define TFT_MADCTL  0x36

#define TFT_MAD_MY  0x80
#define TFT_MAD_MX  0x40
#define TFT_MAD_MV  0x20
#define TFT_MAD_ML  0x10
#define TFT_MAD_RGB 0x00
#define TFT_MAD_BGR 0x08
#define TFT_MAD_MH  0x04
#define TFT_MAD_SS  0x02
#define TFT_MAD_GS  0x01

#define NOP() __asm__ __volatile__ ("nop")
#define digitalWrite gpio_set_level
#define delay(x) vTaskDelay(pdMS_TO_TICKS(x))

#ifndef HIGH
#define HIGH 1
#endif
#ifndef LOW
#define LOW 0
#endif

#define INPUT             0x01
#define OUTPUT            0x03
#define PULLUP            0x04
#define INPUT_PULLUP      0x05
#define OPEN_DRAIN        0x10

/* Register Direct Access Macros */
#define DC_C GPIO.out_w1tc = (1 << TFT_DC)
#define DC_D GPIO.out_w1ts = (1 << TFT_DC)

#define CS_L GPIO.out1_w1tc.val = (1 << (TFT_CS - 32))
#define CS_H GPIO.out1_w1ts.val = (1 << (TFT_CS - 32))

#define WR_L GPIO.out_w1tc = (1 << TFT_WR)
#define WR_H GPIO.out_w1ts = (1 << TFT_WR)

#define RD_L GPIO.out_w1tc = (1 << TFT_RD)
#define RD_H GPIO.out_w1ts = (1 << TFT_RD)

/* Fast parallel bus lookup table: GPIO set mask per data byte, built at
 * compile time so it lives in Flash instead of 1 KB of RAM */
#define XSET_BITS(c) (((c) & 0x01u ? (1u << TFT_D0) : 0u) | ((c) & 0x02u ? (1u << TFT_D1) : 0u) | \
                      ((c) & 0x04u ? (1u << TFT_D2) : 0u) | ((c) & 0x08u ? (1u << TFT_D3) : 0u) | \
                      ((c) & 0x10u ? (1u << TFT_D4) : 0u) | ((c) & 0x20u ? (1u << TFT_D5) : 0u) | \
                      ((c) & 0x40u ? (1u << TFT_D6) : 0u) | ((c) & 0x80u ? (1u << TFT_D7) : 0u))
#define XSET4(n)  XSET_BITS(n), XSET_BITS((n) + 1u), XSET_BITS((n) + 2u), XSET_BITS((n) + 3u)
#define XSET16(n) XSET4(n), XSET4((n) + 4u), XSET4((n) + 8u), XSET4((n) + 12u)
#define XSET64(n) XSET16(n), XSET16((n) + 16u), XSET16((n) + 32u), XSET16((n) + 48u)
static const uint32_t xset_mask[256] = { XSET64(0u), XSET64(64u), XSET64(128u), XSET64(192u) };
static bool locked = true;
static bool inTransaction = false;
static bool lockTransaction = false;

#define GPIO_DIR_MASK ((1 << TFT_D0) | (1 << TFT_D1) | (1 << TFT_D2) | (1 << TFT_D3) | \
                       (1 << TFT_D4) | (1 << TFT_D5) | (1 << TFT_D6) | (1 << TFT_D7))
#define GPIO_OUT_CLR_MASK (GPIO_DIR_MASK | (1 << TFT_WR))

#define set_mask(C) xset_mask[C]

#define tft_Write_8(C)  GPIO.out_w1tc = GPIO_OUT_CLR_MASK; GPIO.out_w1ts = set_mask((uint8_t)(C)); NOP(); WR_H

#define tft_Write_16(C) GPIO.out_w1tc = GPIO_OUT_CLR_MASK; GPIO.out_w1ts = set_mask((uint8_t) ((C) >> 8)); NOP(); WR_H; \
                        GPIO.out_w1tc = GPIO_OUT_CLR_MASK; GPIO.out_w1ts = set_mask((uint8_t) ((C) >> 0)); NOP(); WR_H

#define tft_Write_32C(C,D) GPIO.out_w1tc = GPIO_OUT_CLR_MASK; GPIO.out_w1ts = set_mask((uint8_t) ((C) >> 8)); NOP(); WR_H; \
                           GPIO.out_w1tc = GPIO_OUT_CLR_MASK; GPIO.out_w1ts = set_mask((uint8_t) ((C) >> 0)); NOP(); WR_H; \
                           GPIO.out_w1tc = GPIO_OUT_CLR_MASK; GPIO.out_w1ts = set_mask((uint8_t) ((D) >> 8)); NOP(); WR_H; \
                           GPIO.out_w1tc = GPIO_OUT_CLR_MASK; GPIO.out_w1ts = set_mask((uint8_t) ((D) >> 0)); NOP(); WR_H

static void IRAM_ATTR pinMode(uint8_t pin, uint8_t mode)
{
    if (pin >= SOC_GPIO_PIN_COUNT || (((1ULL << pin) & SOC_GPIO_VALID_GPIO_MASK) == 0)) {
        return;
    }
    gpio_config_t conf = {
        .pin_bit_mask = (1ULL << pin),
        .mode = GPIO_MODE_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    if (mode < 0x20) {
        conf.mode = (gpio_mode_t)(mode & (GPIO_MODE_INPUT | GPIO_MODE_OUTPUT));
        if (mode & OPEN_DRAIN) {
            conf.mode |= GPIO_MODE_DEF_OD;
        }
        if (mode & PULLUP) {
            conf.pull_up_en = GPIO_PULLUP_ENABLE;
        }
        if (mode & 0x08) {
            conf.pull_down_en = GPIO_PULLDOWN_ENABLE;
        }
    }
    gpio_config(&conf);
}

static void IRAM_ATTR delayMicroseconds(uint32_t us)
{
    uint64_t m = (uint64_t)esp_timer_get_time();
    if (us) {
        uint64_t e = (m + us);
        if (m > e) {
            while ((uint64_t)esp_timer_get_time() > e) {
                NOP();
            }
        }
        while ((uint64_t)esp_timer_get_time() < e) {
            NOP();
        }
    }
}

static void begin_tft_write(void) {
    if (locked) {
        locked = false;
        RD_H;
        CS_L;
    }
}

static void end_tft_write(void) {
    if (!inTransaction) {
        if (!locked) {
            locked = true;
            CS_H;
        }
    }
}

static void writecommand(uint8_t c) {
    begin_tft_write();
    DC_C;
    tft_Write_8(c);
    DC_D;
    end_tft_write();
}

static void writedata(uint8_t d) {
    begin_tft_write();
    DC_D;
    tft_Write_8(d);
    CS_L;
    end_tft_write();
}

static void startWrite(void) {
    begin_tft_write();
    lockTransaction = true;
    inTransaction = true;
}

static void endWrite(void) {
    lockTransaction = false;
    inTransaction = false;
    end_tft_write();
}

static void setWindow(int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    DC_C; tft_Write_8(TFT_CASET);
    DC_D; tft_Write_32C(x0, x1);
    DC_C; tft_Write_8(TFT_PASET);
    DC_D; tft_Write_32C(y0, y1);
    DC_C; tft_Write_8(TFT_RAMWR);
    DC_D;
}

static void setAddrWindow(int32_t x0, int32_t y0, int32_t w, int32_t h) {
    begin_tft_write();
    setWindow(x0, y0, x0 + w - 1, y0 + h - 1);
    end_tft_write();
}

static uint8_t s_current_rotation = 0;
static int     s_ili9488_width = ESP32_TFT_WIDTH;
static int     s_ili9488_height = ESP32_TFT_HEIGHT;

static uint8_t madctl_for_rotation(uint8_t r) {
    switch (r) {
        case 1:  return TFT_MAD_MV | TFT_MAD_BGR;
        case 2:  return TFT_MAD_MY | TFT_MAD_BGR;
        case 3:  return TFT_MAD_MX | TFT_MAD_MY | TFT_MAD_MV | TFT_MAD_BGR;
        default: return TFT_MAD_MX | TFT_MAD_BGR;
    }
}

bool esp32_ili9488_dma_active(void);
bool esp32_ili9488_dma_madctl(uint8_t v);

void esp32_ili9488_set_rotation(uint8_t m) {
    s_current_rotation = m % 4;
    s_ili9488_width = (s_current_rotation & 1) ? 480 : 320;
    s_ili9488_height = (s_current_rotation & 1) ? 320 : 480;
    if (esp32_ili9488_dma_madctl(madctl_for_rotation(s_current_rotation))) return;
    begin_tft_write();
    writecommand(TFT_MADCTL);
    switch (s_current_rotation) {
        case 0: /* Portrait (320x480) */
            writedata(TFT_MAD_MX | TFT_MAD_BGR);
            s_ili9488_width = 320;
            s_ili9488_height = 480;
            break;
        case 1: /* Landscape (480x320) */
            writedata(TFT_MAD_MV | TFT_MAD_BGR);
            s_ili9488_width = 480;
            s_ili9488_height = 320;
            break;
        case 2: /* Inverted Portrait (320x480) */
            writedata(TFT_MAD_MY | TFT_MAD_BGR);
            s_ili9488_width = 320;
            s_ili9488_height = 480;
            break;
        case 3: /* Inverted Landscape (480x320) */
            writedata(TFT_MAD_MX | TFT_MAD_MY | TFT_MAD_MV | TFT_MAD_BGR);
            s_ili9488_width = 480;
            s_ili9488_height = 320;
            break;
    }
    delayMicroseconds(10);
    end_tft_write();
}


uint8_t esp32_ili9488_get_rotation(void) {
    return s_current_rotation;
}

int esp32_ili9488_get_width(void) {
    return s_ili9488_width;
}

int esp32_ili9488_get_height(void) {
    return s_ili9488_height;
}

static void tft_initBus(void) {
    pinMode(TFT_CS, OUTPUT);
    digitalWrite(TFT_CS, HIGH);

    pinMode(TFT_WR, OUTPUT);
    digitalWrite(TFT_WR, HIGH);

    pinMode(TFT_DC, OUTPUT);
    digitalWrite(TFT_DC, HIGH);

    pinMode(TFT_RST, OUTPUT);
    digitalWrite(TFT_RST, HIGH);

    pinMode(TFT_RD, OUTPUT);
    digitalWrite(TFT_RD, HIGH);

    pinMode(TFT_D0, OUTPUT); digitalWrite(TFT_D0, HIGH);
    pinMode(TFT_D1, OUTPUT); digitalWrite(TFT_D1, HIGH);
    pinMode(TFT_D2, OUTPUT); digitalWrite(TFT_D2, HIGH);
    pinMode(TFT_D3, OUTPUT); digitalWrite(TFT_D3, HIGH);
    pinMode(TFT_D4, OUTPUT); digitalWrite(TFT_D4, HIGH);
    pinMode(TFT_D5, OUTPUT); digitalWrite(TFT_D5, HIGH);
    pinMode(TFT_D6, OUTPUT); digitalWrite(TFT_D6, HIGH);
    pinMode(TFT_D7, OUTPUT); digitalWrite(TFT_D7, HIGH);

}

static void tft_init(void) {
    pinMode(TFT_CS, OUTPUT);
    digitalWrite(TFT_CS, LOW);

    pinMode(TFT_DC, OUTPUT);
    digitalWrite(TFT_DC, HIGH);

    end_tft_write();

    if (TFT_RST >= 0) {
        writecommand(0x00);
        digitalWrite(TFT_RST, HIGH);
        delay(5);
        digitalWrite(TFT_RST, LOW);
        delay(20);
        digitalWrite(TFT_RST, HIGH);
    }
    delay(150);

    begin_tft_write();

    /* ILI9488 Init Sequence from ILI9488_Init.h */
    writecommand(0xE0); /* Positive Gamma Control */
    writedata(0x00); writedata(0x03); writedata(0x09);
    writedata(0x08); writedata(0x16); writedata(0x0A);
    writedata(0x3F); writedata(0x78); writedata(0x4C);
    writedata(0x09); writedata(0x0A); writedata(0x08);
    writedata(0x16); writedata(0x1A); writedata(0x0F);

    writecommand(0xE1); /* Negative Gamma Control */
    writedata(0x00); writedata(0x16); writedata(0x19);
    writedata(0x03); writedata(0x0F); writedata(0x05);
    writedata(0x32); writedata(0x45); writedata(0x46);
    writedata(0x04); writedata(0x0E); writedata(0x0D);
    writedata(0x35); writedata(0x37); writedata(0x0F);

    writecommand(0xC0); /* Power Control 1 */
    writedata(0x17);
    writedata(0x15);

    writecommand(0xC1); /* Power Control 2 */
    writedata(0x41);

    writecommand(0xC5); /* VCOM Control */
    writedata(0x00);
    writedata(0x12);
    writedata(0x80);

    writecommand(TFT_MADCTL); /* Memory Access Control */
    writedata(0x48);          /* MX, BGR */

    writecommand(0x3A); /* Pixel Interface Format: 16 bit colour for parallel */
    writedata(0x55);

    writecommand(0xB0); /* Interface Mode Control */
    writedata(0x00);

    writecommand(0xB1); /* Frame Rate Control */
    writedata(0xA0);

    writecommand(0xB4); /* Display Inversion Control */
    writedata(0x02);

    writecommand(0xB6); /* Display Function Control */
    writedata(0x02);
    writedata(0x02);
    writedata(0x3B);

    writecommand(0xB7); /* Entry Mode Set */
    writedata(0xC6);

    writecommand(0xF7); /* Adjust Control 3 */
    writedata(0xA9);
    writedata(0x51);
    writedata(0x2C);
    writedata(0x82);

    writecommand(TFT_SLPOUT);  /* Exit Sleep */
    delay(120);

    writecommand(TFT_DISPON);  /* Display on */
    delay(25);

    writecommand(TFT_INVON);   /* Display Inversion ON (IPS panel) */

    end_tft_write();

    esp32_ili9488_set_rotation(0); /* Portrait (320x480) */
}

static uint8_t read_bus(void) {
    uint32_t in = GPIO.in;
    uint8_t val = 0;
    if (in & (1 << TFT_D0)) val |= 0x01;
    if (in & (1 << TFT_D1)) val |= 0x02;
    if (in & (1 << TFT_D2)) val |= 0x04;
    if (in & (1 << TFT_D3)) val |= 0x08;
    if (in & (1 << TFT_D4)) val |= 0x10;
    if (in & (1 << TFT_D5)) val |= 0x20;
    if (in & (1 << TFT_D6)) val |= 0x40;
    if (in & (1 << TFT_D7)) val |= 0x80;
    return val;
}

static void set_bus_direction(uint8_t mode) {
    pinMode(TFT_D0, mode);
    pinMode(TFT_D1, mode);
    pinMode(TFT_D2, mode);
    pinMode(TFT_D3, mode);
    pinMode(TFT_D4, mode);
    pinMode(TFT_D5, mode);
    pinMode(TFT_D6, mode);
    pinMode(TFT_D7, mode);
}

static void read_cmd(uint8_t cmd, uint8_t *out_bytes, int count, bool dummy_read) {
    startWrite();
    DC_C;
    tft_Write_8(cmd);
    DC_D;

    set_bus_direction(INPUT);

    if (dummy_read) {
        /* Dummy read */
        RD_L;
        NOP(); NOP(); NOP(); NOP(); NOP(); NOP();
        RD_H;
        NOP(); NOP();
    }

    for (int i = 0; i < count; i++) {
        RD_L;
        NOP(); NOP(); NOP(); NOP(); NOP(); NOP();
        out_bytes[i] = read_bus();
        RD_H;
        NOP(); NOP();
    }

    set_bus_direction(OUTPUT);
    endWrite();
}

void esp32_ili9488_init(void) {
    tft_initBus();
    tft_init();

    /* Read ILI9488 Diagnostic Registers */
    uint8_t id_d3[4] = {0};
    uint8_t id_04[4] = {0};
    uint8_t pwr_0a[1] = {0};
    uint8_t mad_0b[1] = {0};
    uint8_t pix_0c[1] = {0};

    read_cmd(0xD3, id_d3, 4, true);
    read_cmd(0x04, id_04, 4, true);
    read_cmd(0x0A, pwr_0a, 1, false);
    read_cmd(0x0B, mad_0b, 1, false);
    read_cmd(0x0C, pix_0c, 1, false);

    printf("[ILI9488 DIAG] 0xD3 Device ID: 0x%02X 0x%02X 0x%02X 0x%02X (Expected: xx xx 94 88)\n",
           id_d3[0], id_d3[1], id_d3[2], id_d3[3]);
    printf("[ILI9488 DIAG] 0x04 Display ID: 0x%02X 0x%02X 0x%02X 0x%02X\n",
           id_04[0], id_04[1], id_04[2], id_04[3]);
    printf("[ILI9488 DIAG] 0x0A Power Mode: 0x%02X | 0x0B MADCTL: 0x%02X | 0x0C PixFmt: 0x%02X\n",
           pwr_0a[0], mad_0b[0], pix_0c[0]);

    /* Display RGBW diagnostic test bands on power-up */
    startWrite();
    setAddrWindow(0, 0, ESP32_TFT_WIDTH, ESP32_TFT_HEIGHT);
    int band_h = ESP32_TFT_HEIGHT / 4;
    for (int y = 0; y < ESP32_TFT_HEIGHT; y++) {
        uint16_t c;
        if (y < band_h) c = 0xF800;             /* Red */
        else if (y < band_h * 2) c = 0x07E0;    /* Green */
        else if (y < band_h * 3) c = 0x001F;    /* Blue */
        else c = 0xFFFF;                        /* White */
        for (int x = 0; x < ESP32_TFT_WIDTH; x++) {
            tft_Write_16(c);
        }
    }
    endWrite();
    delay(400);

    /* Keep RGBW diagnostic test pattern on display until GUI flush */
}

void esp32_ili9488_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    setWindow(x0, y0, x1, y1);
}

void esp32_ili9488_flush(const uint16_t *vfb, int width, int height) {
    if (width <= 0 || height <= 0) return;
    startWrite();
    setAddrWindow(0, 0, width, height);
    if (vfb) {
        int total = width * height;
        for (int i = 0; i < total; i++) {
            tft_Write_16(vfb[i]);
        }
    } else {
        int non_zero = 0;
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                uint16_t color = mini_gui_get_pixel(x, y);
                if (color != 0) non_zero++;
                tft_Write_16(color);
            }
        }
        printf("[ILI9488 FLUSH] Flushed %dx%d via get_pixel. Non-zero: %d\n", width, height, non_zero);
    }
    endWrite();
}

void esp32_ili9488_flush_rect(const uint16_t *vfb, int stride, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    startWrite();
    setAddrWindow(x, y, w, h);
    if (vfb) {
        for (int r = 0; r < h; r++) {
            const uint16_t *row = &vfb[(y + r) * stride + x];
            for (int c = 0; c < w; c++) {
                tft_Write_16(row[c]);
            }
        }
    } else {
        for (int r = 0; r < h; r++) {
            for (int c = 0; c < w; c++) {
                uint16_t color = mini_gui_get_pixel(x + c, y + r);
                tft_Write_16(color);
            }
        }
    }
    endWrite();
}

void esp32_ili9488_set_pixel_format(uint8_t fmt) {
    startWrite();
    writecommand(0x3A);
    writedata(fmt);
    endWrite();
}

/* ------------------------------------------------------------------------- */
/* I2S (i80 LCD mode) DMA transport, used once the panel is initialized.     */
/* The bit-banged init sequence above runs first; start_dma then hands the   */
/* pins to the I2S peripheral. tx_color copies the band into the driver's    */
/* DMA buffer and returns while the transfer runs, so the next band renders  */
/* in parallel with the bus.                                                 */
/* ------------------------------------------------------------------------- */
/* RGB565 -> R, G, B bytes (the panel keeps the 6 MSBs of each) */
static inline void rgb565_to_666(uint16_t c, uint8_t *out) {
    uint8_t r = (uint8_t)(c >> 11), g = (uint8_t)((c >> 5) & 63), b = (uint8_t)(c & 31);
    out[0] = (uint8_t)((r << 3) | (r >> 2));
    out[1] = (uint8_t)((g << 2) | (g >> 4));
    out[2] = (uint8_t)((b << 3) | (b >> 2));
}

#if defined(ESP_PLATFORM) && __has_include("esp_lcd_io_i80.h")
#include "esp_lcd_io_i80.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_rom_gpio.h"
#include "soc/gpio_sig_map.h"
#include "esp_rom_sys.h"
#define ILI9488_HAS_I80 1
static esp_lcd_i80_bus_handle_t s_i80_bus = NULL;
static esp_lcd_panel_io_handle_t s_i80_io = NULL;
static size_t s_dma_ram_bytes = 0;
static size_t s_max_transfer = 0;   /* bytes per DMA transfer (one chunk) */

static void i80_set_window(int x, int y, int w, int h) {
    uint8_t caset[4] = { (uint8_t)(x >> 8), (uint8_t)x, (uint8_t)((x + w - 1) >> 8), (uint8_t)(x + w - 1) };
    uint8_t paset[4] = { (uint8_t)(y >> 8), (uint8_t)y, (uint8_t)((y + h - 1) >> 8), (uint8_t)(y + h - 1) };
    esp_lcd_panel_io_tx_param(s_i80_io, TFT_CASET, caset, 4);
    esp_lcd_panel_io_tx_param(s_i80_io, TFT_PASET, paset, 4);
}

static bool i80_open(size_t max_transfer_bytes, uint32_t pclk_hz, bool swap_bytes) {
    gpio_set_level((gpio_num_t)TFT_RD, 1); /* read strobe stays inactive */
    esp_lcd_i80_bus_config_t bus_config = {
        .dc_gpio_num = TFT_DC,
        .wr_gpio_num = TFT_WR,
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .data_gpio_nums = { TFT_D0, TFT_D1, TFT_D2, TFT_D3, TFT_D4, TFT_D5, TFT_D6, TFT_D7 },
        .bus_width = 8,
        .max_transfer_bytes = max_transfer_bytes,
    };
    esp_err_t err = esp_lcd_new_i80_bus(&bus_config, &s_i80_bus);
    if (err != ESP_OK) {
        ESP_LOGW("ILI9488", "i80 bus: %s", esp_err_to_name(err));
        s_i80_bus = NULL;
        return false;
    }
    esp_lcd_panel_io_i80_config_t io_config = {
        .cs_gpio_num = TFT_CS,
        .pclk_hz = pclk_hz,
        .trans_queue_depth = 4,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .dc_levels = { .dc_idle_level = 0, .dc_cmd_level = 0, .dc_dummy_level = 0, .dc_data_level = 1 },
        /* RGB565 words go high byte first; RGB666 bytes are already in order */
        .flags = { .swap_color_bytes = swap_bytes ? 1 : 0 },
    };
    /* classic ESP32 I2S: one 4-byte FIFO word per 2 bus bytes (see esp_lcd) */
    s_dma_ram_bytes = ((max_transfer_bytes + 3) & ~(size_t)3) * 2 + 4;
    s_max_transfer = max_transfer_bytes;
    err = esp_lcd_new_panel_io_i80(s_i80_bus, &io_config, &s_i80_io);
    if (err != ESP_OK) {
        ESP_LOGW("ILI9488", "i80 panel IO: %s", esp_err_to_name(err));
        esp_lcd_del_i80_bus(s_i80_bus);
        s_i80_bus = NULL;
        s_i80_io = NULL;
        return false;
    }
    return true;
}

/* Releases the I2S peripheral and routes the pins back to GPIO bit-bang */
static void i80_close(void) {
    if (s_i80_io) esp_lcd_panel_io_del(s_i80_io);
    if (s_i80_bus) esp_lcd_del_i80_bus(s_i80_bus);
    s_i80_io = NULL;
    s_i80_bus = NULL;
    static const uint8_t pins[] = { TFT_D0, TFT_D1, TFT_D2, TFT_D3, TFT_D4, TFT_D5, TFT_D6, TFT_D7, TFT_WR, TFT_DC, TFT_CS };
    for (size_t i = 0; i < sizeof(pins); i++) {
        esp_rom_gpio_connect_out_signal(pins[i], SIG_GPIO_OUT_IDX, false, false);
    }
    tft_initBus();
    locked = true;
    inTransaction = false;
    CS_H;
}

/* Frame memory read: ILI9488 returns 3 bytes (R, G, B; 6 MSBs valid) per
 * pixel whatever the write format. RAMRD needs a slow (>= 355 ns) strobe. */
static void read_ram(int x, int y, int n, uint8_t *out) {
    startWrite();
    DC_C; tft_Write_8(TFT_CASET); DC_D; tft_Write_32C(x, x + n - 1);
    DC_C; tft_Write_8(TFT_PASET); DC_D; tft_Write_32C(y, y);
    DC_C; tft_Write_8(TFT_RAMRD); DC_D;
    set_bus_direction(INPUT);
    for (int i = -1; i < n * 3; i++) { /* i == -1: dummy read */
        RD_L;
        esp_rom_delay_us(1);
        uint8_t v = read_bus();
        RD_H;
        esp_rom_delay_us(1);
        if (i >= 0) out[i] = v;
    }
    set_bus_direction(OUTPUT);
    endWrite();
}

#define PROBE_PIXELS 12
static const uint16_t s_probe[PROBE_PIXELS] = {
    0xF800, 0x07E0, 0x001F, 0xFFFF, 0x0000, 0xA554,
    0x52AA, 0x1234, 0xEDCB, 0x8410, 0x0821, 0x7BEF
};

/* Counts probe pixels found in panel RAM at row 0 (R/B order either way) */
static int probe_matches(void) {
    uint8_t rgb[PROBE_PIXELS * 3];
    read_ram(0, 0, PROBE_PIXELS, rgb);
    int ok_rgb = 0, ok_bgr = 0;
    for (int i = 0; i < PROBE_PIXELS; i++) {
        unsigned r5 = s_probe[i] >> 11, g6 = (s_probe[i] >> 5) & 63, b5 = s_probe[i] & 31;
        unsigned a = rgb[i * 3] >> 3, g = rgb[i * 3 + 1] >> 2, c = rgb[i * 3 + 2] >> 3;
        if (g == g6 && a == r5 && c == b5) ok_rgb++;
        if (g == g6 && a == b5 && c == r5) ok_bgr++;
    }
    if (ok_rgb + ok_bgr < PROBE_PIXELS) {
        ESP_LOGI("ILI9488", "probe read: %02X %02X %02X  %02X %02X %02X  %02X %02X %02X",
                 rgb[0], rgb[1], rgb[2], rgb[3], rgb[4], rgb[5], rgb[6], rgb[7], rgb[8]);
    }
    return ok_rgb > ok_bgr ? ok_rgb : ok_bgr;
}

bool esp32_ili9488_start_dma(size_t max_transfer_bytes, uint32_t pclk_hz, bool rgb666) {
    if (s_i80_io) return true;
    /* Control run: bit-bang write, read back. Proves the read path works. */
    startWrite();
    setAddrWindow(0, 0, PROBE_PIXELS, 1);
    for (int i = 0; i < PROBE_PIXELS; i++) { tft_Write_16(s_probe[i]); }
    endWrite();
    int control = probe_matches();
    ESP_LOGI("ILI9488", "readback control (GPIO write): %d/%d pixels", control, PROBE_PIXELS);
    bool can_verify = (control == PROBE_PIXELS);

    /* WR clock is 40 MHz / n on the I2S bus: step n up until writes land */
    for (uint32_t n = 40000000u / pclk_hz; n == 0 || 40000000u / n >= 2000000u; n++) {
        if (n == 0) continue;
        uint32_t hz = 40000000u / n;
        /* clear the probe row so a stale control pattern cannot pass */
        startWrite();
        setAddrWindow(0, 0, PROBE_PIXELS, 1);
        for (int i = 0; i < PROBE_PIXELS; i++) { tft_Write_16(0x39E7); }
        endWrite();
        if (!i80_open(max_transfer_bytes, hz, true)) return false;
        if (!can_verify) {
            if (hz > 10000000u) {
                i80_close();
                continue;
            }
            ESP_LOGW("ILI9488", "panel readback unusable, trusting DMA at %u Hz unverified", (unsigned)hz);
            return true;
        }
        i80_set_window(0, 0, PROBE_PIXELS, 1);
        esp_err_t err = esp_lcd_panel_io_tx_color(s_i80_io, TFT_RAMWR, s_probe, sizeof(s_probe));
        esp_lcd_panel_io_tx_param(s_i80_io, TFT_NOP, NULL, 0); /* waits for the color transfer */
        i80_close();
        int got = probe_matches();
        ESP_LOGI("ILI9488", "DMA probe at %u Hz: %d/%d pixels (%s)", (unsigned)hz, got, PROBE_PIXELS,
                 esp_err_to_name(err));
        if (got == PROBE_PIXELS && rgb666) {
            /* Same check in the RGB666 byte order the bands will use */
            startWrite();
            setAddrWindow(0, 0, PROBE_PIXELS, 1);
            for (int i = 0; i < PROBE_PIXELS; i++) { tft_Write_16(0x39E7); }
            endWrite();
            if (!i80_open(max_transfer_bytes, hz, false)) return false;
            uint8_t fmt = 0x66, rgb[PROBE_PIXELS * 3];
            for (int i = 0; i < PROBE_PIXELS; i++) rgb565_to_666(s_probe[i], &rgb[i * 3]);
            esp_lcd_panel_io_tx_param(s_i80_io, 0x3A, &fmt, 1);
            i80_set_window(0, 0, PROBE_PIXELS, 1);
            esp_lcd_panel_io_tx_color(s_i80_io, TFT_RAMWR, rgb, sizeof(rgb));
            esp_lcd_panel_io_tx_param(s_i80_io, TFT_NOP, NULL, 0);
            i80_close();
            got = probe_matches();
            ESP_LOGI("ILI9488", "RGB666 DMA probe at %u Hz: %d/%d pixels", (unsigned)hz, got, PROBE_PIXELS);
            esp32_ili9488_set_pixel_format(0x55);
        }
        if (got == PROBE_PIXELS && i80_open(max_transfer_bytes, hz, !rgb666)) {
            ESP_LOGI("ILI9488", "I2S DMA bus active: %u Hz WR clock, %u B per transfer",
                     (unsigned)hz, (unsigned)max_transfer_bytes);
            return true;
        }
    }
    ESP_LOGW("ILI9488", "DMA writes do not reach the panel, keeping GPIO bit-bang");
    return false;
}

bool esp32_ili9488_dma_active(void) {
    return s_i80_io != NULL;
}

static uint8_t *s_conv = NULL;      /* RGB666 staging for one chunk */
static size_t s_conv_bytes = 0;

size_t esp32_ili9488_dma_ram_bytes(void) {
    return s_i80_io ? s_dma_ram_bytes + s_conv_bytes : 0;
}

bool esp32_ili9488_dma_madctl(uint8_t v) {
    if (!s_i80_io) return false;
    esp_lcd_panel_io_tx_param(s_i80_io, TFT_MADCTL, &v, 1);
    return true;
}
#else
bool esp32_ili9488_start_dma(size_t max_transfer_bytes, uint32_t pclk_hz, bool rgb666) {
    (void)max_transfer_bytes; (void)pclk_hz; (void)rgb666;
    return false;
}
bool esp32_ili9488_dma_active(void) { return false; }
size_t esp32_ili9488_dma_ram_bytes(void) { return 0; }
bool esp32_ili9488_dma_madctl(uint8_t v) { (void)v; return false; }
#endif

#define TFT_RAMWRC  0x3C /* memory write continue */

static bool s_bus_666 = false;

/* Panel pixel format on the bus: RGB666 sends 3 bytes per pixel */
void esp32_ili9488_set_bus_rgb666(bool on) {
    s_bus_666 = on;
    uint8_t fmt = on ? 0x66 : 0x55;
#if defined(ILI9488_HAS_I80)
    if (s_i80_io) {
        esp_lcd_panel_io_tx_param(s_i80_io, 0x3A, &fmt, 1);
        if (on && !s_conv) {
            s_conv = (uint8_t *)heap_caps_malloc(s_max_transfer, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            s_conv_bytes = s_conv ? s_max_transfer : 0;
        }
        return;
    }
#endif
    esp32_ili9488_set_pixel_format(fmt);
}

bool esp32_ili9488_bus_rgb666(void) {
    return s_bus_666;
}

#if defined(ILI9488_HAS_I80)
static void i80_rows_by_line(const MiniGuiRows *rows, int x0, int width, int y, int h);
#endif

/* Sends a full-width RGB565 band whose first row is screen row y. With DMA
 * the band goes out in chunks of whole rows that fit one transfer (the first
 * with RAMWR, the rest with RAMWRC); each call returns once its data is
 * copied, so the next chunk is prepared while the previous one is on the bus. */
static void flush_rgb565(const uint16_t *pixels, int x0, int width, int y, int h) {
    if (!pixels || width <= 0 || h <= 0) return;
    size_t bpp = s_bus_666 ? 3 : 2;
#if defined(ILI9488_HAS_I80)
    if (s_i80_io && (!s_bus_666 || s_conv)) {
        int chunk = (int)(s_max_transfer / ((size_t)width * bpp));
        if (chunk < 1) chunk = 1;
        i80_set_window(x0, y, width, h);
        for (int r = 0; r < h; r += chunk) {
            int n = (r + chunk > h) ? (h - r) : chunk;
            const uint16_t *src = pixels + (size_t)r * (size_t)width;
            int cmd = (r == 0) ? TFT_RAMWR : TFT_RAMWRC;
            if (s_bus_666) {
                size_t count = (size_t)n * (size_t)width;
                for (size_t i = 0; i < count; i++) rgb565_to_666(src[i], &s_conv[i * 3]);
                esp_lcd_panel_io_tx_color(s_i80_io, cmd, s_conv, count * 3);
            } else {
                esp_lcd_panel_io_tx_color(s_i80_io, cmd, src, (size_t)n * (size_t)width * 2);
            }
        }
        return;
    }
    if (s_i80_io) { /* RGB666 bus, no staging buffer */
        MiniGuiRows rows = { (const uint8_t *)pixels, (size_t)width * 2, 16, NULL };
        i80_rows_by_line(&rows, x0, width, y, h);
        return;
    }
#endif
    startWrite();
    setAddrWindow(x0, y, width, h);
    const uint16_t *p = pixels;
    if (s_bus_666) {
        uint8_t rgb[3];
        for (int n = width * h; n > 0; n--) {
            rgb565_to_666(*p++, rgb);
            tft_Write_8(rgb[0]);
            tft_Write_8(rgb[1]);
            tft_Write_8(rgb[2]);
        }
    } else {
        for (int n = width * h; n > 0; n--) {
            tft_Write_16(*p++);
        }
    }
    endWrite();
}

void esp32_ili9488_flush_band(const uint16_t *pixels, int width, int y, int h) {
    flush_rgb565(pixels, 0, width, y, h);
}

size_t esp32_ili9488_reserve_staging(void) {
#if defined(ILI9488_HAS_I80)
    if (s_i80_io && !s_conv) {
        s_conv = (uint8_t *)heap_caps_malloc(s_max_transfer, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        s_conv_bytes = s_conv ? s_max_transfer : 0;
    }
    return s_conv_bytes;
#else
    return 0;
#endif
}

#if defined(ILI9488_HAS_I80)
/* Rows over DMA without the staging buffer (no RAM for it): one row at a
 * time from the stack; the driver copies each before tx_color returns. The
 * GPIO bit-bang path must never run while the i80 bus owns the pins: those
 * writes do not reach the panel and the bus stops working until a reboot. */
static void __attribute__((noinline)) i80_rows_by_line(const MiniGuiRows *rows, int x0, int width, int y, int h) {
    uint16_t line[480];
    uint8_t row666[480 * 3];
    i80_set_window(x0, y, width, h);
    for (int r = 0; r < h; r++) {
        mini_gui_rows_to_rgb565(rows, r, 0, width, line);
        const void *src = line;
        size_t n = (size_t)width * 2;
        if (s_bus_666) {
            for (int x = 0; x < width; x++) rgb565_to_666(line[x], &row666[x * 3]);
            src = row666;
            n = (size_t)width * 3;
        }
        esp_lcd_panel_io_tx_color(s_i80_io, r == 0 ? TFT_RAMWR : TFT_RAMWRC, src, n);
    }
}
#endif

/* Rows of any color bits: RGB565 on an RGB565 bus goes out as is; anything
 * else is expanded through the palette (and to RGB666) chunk by chunk into
 * the staging buffer, which the driver copies before tx_color returns. */
void esp32_ili9488_flush_rows(const MiniGuiRows *rows, int width, int y, int h) {
    esp32_ili9488_flush_rows_at(rows, 0, y, width, h);
}

/* Rows `width` pixels wide for screen columns [x0, x0 + width) */
void esp32_ili9488_flush_rows_at(const MiniGuiRows *rows, int x0, int y, int width, int h) {
    if (!rows || !rows->data || width <= 0 || h <= 0) return;
    if (rows->bits == 16 && rows->stride == (size_t)width * 2 && !s_bus_666) {
#if defined(ILI9488_HAS_I80)
        if (s_conv) { /* back to RGB565 rows: the staging buffer is not needed */
            heap_caps_free(s_conv);
            s_conv = NULL;
            s_conv_bytes = 0;
        }
#endif
        flush_rgb565((const uint16_t *)(const void *)rows->data, x0, width, y, h);
        return;
    }
    uint16_t line[480];
    if (width > 480) return;
    size_t bpp = s_bus_666 ? 3 : 2;
#if defined(ILI9488_HAS_I80)
    /* fewer color bits set at run time (WebREPL, gui-color-bits): the
     * staging buffer is made on first need, as at boot */
    if (s_i80_io && !s_conv) esp32_ili9488_reserve_staging();
    if (s_i80_io && !s_conv) {
        i80_rows_by_line(rows, x0, width, y, h);
        return;
    }
    if (s_i80_io && s_conv) {
        int chunk = (int)(s_max_transfer / ((size_t)width * bpp));
        if (chunk < 1) chunk = 1;
        i80_set_window(x0, y, width, h);
        for (int r = 0; r < h; r += chunk) {
            int n = (r + chunk > h) ? (h - r) : chunk;
            for (int i = 0; i < n; i++) {
                if (s_bus_666) {
                    mini_gui_rows_to_rgb565(rows, r + i, 0, width, line);
                    uint8_t *out = &s_conv[(size_t)i * (size_t)width * 3];
                    for (int x = 0; x < width; x++) rgb565_to_666(line[x], &out[x * 3]);
                } else {
                    mini_gui_rows_to_rgb565(rows, r + i, 0, width, (uint16_t *)(void *)&s_conv[(size_t)i * (size_t)width * 2]);
                }
            }
            esp_lcd_panel_io_tx_color(s_i80_io, r == 0 ? TFT_RAMWR : TFT_RAMWRC, s_conv, (size_t)n * (size_t)width * bpp);
        }
        return;
    }
#endif
    startWrite();
    setAddrWindow(x0, y, width, h);
    for (int r = 0; r < h; r++) {
        mini_gui_rows_to_rgb565(rows, r, 0, width, line);
        for (int x = 0; x < width; x++) {
            if (s_bus_666) {
                uint8_t rgb[3];
                rgb565_to_666(line[x], rgb);
                tft_Write_8(rgb[0]);
                tft_Write_8(rgb[1]);
                tft_Write_8(rgb[2]);
            } else {
                tft_Write_16(line[x]);
            }
        }
    }
    endWrite();
}

/* One solid rectangle (DIRECT mode). With DMA every fill costs a window
 * command pair plus one transfer per FILL_PX pixels; bit-bang costs a few
 * bus cycles per pixel and nothing per call. */
#define FILL_PX 256
void esp32_ili9488_fill(int x, int y, int w, int h, uint16_t rgb565) {
    if (w <= 0 || h <= 0) return;
    size_t total = (size_t)w * (size_t)h;
    uint8_t rgb[3];
    rgb565_to_666(rgb565, rgb);
#if defined(ILI9488_HAS_I80)
    if (s_i80_io) {
        static uint8_t s_fill[FILL_PX * 3];
        static uint32_t s_fill_key = 0xFFFFFFFFu;
        uint32_t key = ((uint32_t)s_bus_666 << 16) | rgb565;
        size_t bpp = s_bus_666 ? 3 : 2;
        if (key != s_fill_key) {
            for (int i = 0; i < FILL_PX; i++) {
                if (s_bus_666) memcpy(&s_fill[i * 3], rgb, 3);
                else memcpy(&s_fill[i * 2], &rgb565, 2);
            }
            s_fill_key = key;
        }
        i80_set_window(x, y, w, h);
        for (size_t done = 0; done < total; done += FILL_PX) {
            size_t n = total - done < FILL_PX ? total - done : FILL_PX;
            esp_lcd_panel_io_tx_color(s_i80_io, done == 0 ? TFT_RAMWR : TFT_RAMWRC, s_fill, n * bpp);
        }
        return;
    }
#endif
    startWrite();
    setAddrWindow(x, y, w, h);
    if (s_bus_666) {
        for (size_t i = 0; i < total; i++) {
            tft_Write_8(rgb[0]);
            tft_Write_8(rgb[1]);
            tft_Write_8(rgb[2]);
        }
    } else {
        for (size_t i = 0; i < total; i++) {
            tft_Write_16(rgb565);
        }
    }
    endWrite();
}

void esp32_ili9488_flush_rect_rgb666(const uint32_t *pixels, int stride, int x, int y, int w, int h) {
    if (!pixels || w <= 0 || h <= 0) return;
    startWrite();
    setAddrWindow(x, y, w, h);
    for (int r = 0; r < h; r++) {
        const uint32_t *row = &pixels[r * stride];
        for (int c = 0; c < w; c++) {
            uint32_t p = row[c];
            tft_Write_8((uint8_t)(p >> 16)); /* Red */
            tft_Write_8((uint8_t)(p >> 8));  /* Green */
            tft_Write_8((uint8_t)(p >> 0));  /* Blue */
        }
    }
    endWrite();
}

void esp32_ili9488_flush_dirty(const uint16_t *vfb, int stride) {
    if (!mini_gui_is_dirty()) return;
    int count = mini_gui_get_dirty_rect_count();
    static int s_dirty_log_count = 0;
    if (++s_dirty_log_count <= 5 || s_dirty_log_count % 60 == 0) {
        printf("[ILI9488 DIRTY] Flushing %d rects (stride=%d)\n", count, stride);
    }
    for (int i = 0; i < count; i++) {
        int x = 0, y = 0, w = 0, h = 0;
        if (mini_gui_get_dirty_rect_at(i, &x, &y, &w, &h)) {
            esp32_ili9488_flush_rect(vfb, stride, x, y, w, h);
        }
    }
    mini_gui_clear_dirty();
}

/* Double buffer stub for DMA band transfer (unused on parallel bus) */
void esp32_ili9488_dma_init(void) {}
void esp32_ili9488_dma_wait(void) {}

void esp32_ili9488_flush_band_dma(int band_index, const uint16_t *pixels, int count) {
    if (!pixels || count <= 0) return;
    int y0 = band_index * ESP32_DMA_BAND_HEIGHT;
    int y1 = y0 + (count / ESP32_TFT_WIDTH) - 1;
    if (y1 >= ESP32_TFT_HEIGHT) y1 = ESP32_TFT_HEIGHT - 1;

    startWrite();
    setWindow(0, y0, ESP32_TFT_WIDTH - 1, y1);
    for (int i = 0; i < count; i++) {
        tft_Write_16(pixels[i]);
    }
    endWrite();
}

/* Backlight Control via LEDC PWM */
void esp32_backlight_init(void) {
    gpio_set_direction(ESP32_PIN_BL, GPIO_MODE_OUTPUT);
    gpio_set_level(ESP32_PIN_BL, 1);

    ledc_timer_config_t timer_conf = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK
    };
    ledc_timer_config(&timer_conf);

    ledc_channel_config_t ch_conf = {
        .gpio_num = ESP32_PIN_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 255,
        .hpoint = 0
    };
    ledc_channel_config(&ch_conf);
}

void esp32_backlight_set(uint8_t brightness) {
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, brightness);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

/* Photoresistor ADC (GPIO 35) */
#if __has_include("esp_adc/adc_oneshot.h")
void esp32_light_init(void) {
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    adc_oneshot_new_unit(&init_config, &s_adc_handle);

    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_12,
        .atten = ADC_ATTEN_DB_12,
    };
    adc_oneshot_config_channel(s_adc_handle, ADC_CHANNEL_7, &config);
}

int esp32_light_read(void) {
    int val = 0;
    if (s_adc_handle) {
        adc_oneshot_read(s_adc_handle, ADC_CHANNEL_7, &val);
    }
    return val;
}
/* ADC1 channel of a GPIO: 36..39 are channels 0..3, 32..35 channels 4..7 */
static int adc1_channel(int pin) {
    if (pin >= 36 && pin <= 39) return pin - 36;
    if (pin >= 32 && pin <= 35) return pin - 28;
    return -1;
}

int esp32_adc_read(int pin) {
    static uint8_t configured;   /* bit per channel set up for 0..3.3 V */
    int ch = adc1_channel(pin), val = 0;
    if (ch < 0 || !s_adc_handle) return -1;
    if (!(configured & (1u << ch))) {
        adc_oneshot_chan_cfg_t config = { .bitwidth = ADC_BITWIDTH_12, .atten = ADC_ATTEN_DB_12 };
        if (adc_oneshot_config_channel(s_adc_handle, (adc_channel_t)ch, &config) != ESP_OK) return -1;
        configured |= (uint8_t)(1u << ch);
    }
    return adc_oneshot_read(s_adc_handle, (adc_channel_t)ch, &val) == ESP_OK ? val : -1;
}
#elif __has_include("driver/adc.h")
int esp32_adc_read(int pin) { (void)pin; return -1; }

void esp32_light_init(void) {
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(ADC1_CHANNEL_7, ADC_ATTEN_DB_11);
}

int esp32_light_read(void) {
    return adc1_get_raw(ADC1_CHANNEL_7);
}
#else
void esp32_light_init(void) {}
int esp32_light_read(void) { return 2048; }
int esp32_adc_read(int pin) { (void)pin; return -1; }
#endif

/* PWM for apps: LEDC timer 1 (one frequency for all of them, set by the
 * latest call) and channels 1..7, one per pin; 8-bit duty */
bool esp32_pwm_write(int pin, int freq_hz, int duty) {
    static int8_t chan_pin[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };   /* channel 0: the backlight */
    static int timer_hz = 0;
    if (pin < 0 || pin > 33 || pin == ESP32_PIN_BL) return false;
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
    if (ch < 0) return false;                       /* all 7 channels in use */
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

/* User Button (GPIO 18, active-high with 10k pulldown to GND). An edge
 * interrupt counts presses: the main loop reads the level once per pass,
 * and a short press during a long frame would otherwise go unseen. */
static volatile uint32_t s_btn_presses = 0;
static volatile int64_t s_btn_low_since = 0;

static void IRAM_ATTR button_isr(void *arg) {
    (void)arg;
    int64_t now = esp_timer_get_time();
    if (REG_READ(GPIO_IN_REG) & BIT(ESP32_PIN_BTN)) {
        /* a press: only after 30 ms released, so contact bounce counts once */
        if (now - s_btn_low_since > 30000) s_btn_presses++;
    } else {
        s_btn_low_since = now;
    }
}

void esp32_button_init(void) {
    gpio_config_t btn_conf = {
        .pin_bit_mask = (1ULL << ESP32_PIN_BTN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_ANYEDGE
    };
    gpio_config(&btn_conf);
    esp_err_t err = gpio_install_isr_service(0);
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) gpio_isr_handler_add(ESP32_PIN_BTN, button_isr, NULL);
}

uint32_t esp32_button_presses(void) {
    return s_btn_presses;
}

bool esp32_button_read(void) {
    return gpio_get_level(ESP32_PIN_BTN) == 1;
}

/* WS2812 Bitbang on GPIO 21 */
void esp32_ws2812_init(void) {
    gpio_set_direction(ESP32_PIN_WS2812, GPIO_MODE_OUTPUT);
    gpio_set_level(ESP32_PIN_WS2812, 0);
}

void esp32_ws2812_set(uint8_t r, uint8_t g, uint8_t b) {
    uint8_t bytes[3] = { g, r, b };
    portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    taskENTER_CRITICAL(&mux);
    for (int i = 0; i < 3; i++) {
        uint8_t byte = bytes[i];
        for (int bit = 7; bit >= 0; bit--) {
            if (byte & (1 << bit)) {
                GPIO.out_w1ts = (1 << ESP32_PIN_WS2812);
                __asm__ __volatile__ ("nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;");
                GPIO.out_w1tc = (1 << ESP32_PIN_WS2812);
                __asm__ __volatile__ ("nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;");
            } else {
                GPIO.out_w1ts = (1 << ESP32_PIN_WS2812);
                __asm__ __volatile__ ("nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;");
                GPIO.out_w1tc = (1 << ESP32_PIN_WS2812);
                __asm__ __volatile__ ("nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;nop;");
            }
        }
    }
    taskEXIT_CRITICAL(&mux);
}

#else

/* Simulation / Host Stubs for Native Compilation & Unit Tests */
void esp32_ili9488_init(void) {}
void esp32_ili9488_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    (void)x0; (void)y0; (void)x1; (void)y1;
}
void esp32_ili9488_flush(const uint16_t *vfb, int width, int height) {
    (void)vfb; (void)width; (void)height;
}
void esp32_ili9488_flush_rect(const uint16_t *vfb, int stride, int x, int y, int w, int h) {
    (void)vfb; (void)stride; (void)x; (void)y; (void)w; (void)h;
}
void esp32_ili9488_flush_dirty(const uint16_t *vfb, int stride) {
    (void)vfb; (void)stride;
    mini_gui_clear_dirty();
}
void esp32_ili9488_set_pixel_format(uint8_t fmt) { (void)fmt; }
bool esp32_ili9488_start_dma(size_t max_transfer_bytes, uint32_t pclk_hz, bool rgb666) {
    (void)max_transfer_bytes; (void)pclk_hz; (void)rgb666;
    return false;
}
void esp32_ili9488_set_bus_rgb666(bool on) { (void)on; }
bool esp32_ili9488_bus_rgb666(void) { return false; }
bool esp32_ili9488_dma_active(void) { return false; }
void esp32_ili9488_flush_band(const uint16_t *pixels, int width, int y, int h) {
    (void)pixels; (void)width; (void)y; (void)h;
}
size_t esp32_ili9488_reserve_staging(void) { return 0; }
void esp32_ili9488_flush_rows(const MiniGuiRows *rows, int width, int y, int h) {
    (void)rows; (void)width; (void)y; (void)h;
}
void esp32_ili9488_flush_rows_at(const MiniGuiRows *rows, int x0, int y, int width, int h) {
    (void)rows; (void)x0; (void)y; (void)width; (void)h;
}
void esp32_ili9488_fill(int x, int y, int w, int h, uint16_t rgb565) {
    (void)x; (void)y; (void)w; (void)h; (void)rgb565;
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
void esp32_ws2812_init(void) {}
void esp32_ws2812_set(uint8_t r, uint8_t g, uint8_t b) { (void)r; (void)g; (void)b; }
void esp32_light_init(void) {}
int  esp32_light_read(void) { return 2048; }
void esp32_button_init(void) {}
bool esp32_button_read(void) { return false; }
uint32_t esp32_button_presses(void) { return 0; }

#endif

void esp32_hardware_poll_and_sync(MiniVM *vm) {
    (void)vm;
    mini_button_set(esp32_button_read());
    mini_light_set(esp32_light_read());

    if (mini_gui_is_dirty()) {
        esp32_ili9488_flush_dirty(mini_gui_get_vfb(), mini_gui_get_width());
    }
}
