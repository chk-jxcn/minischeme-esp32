/**
 * @file minischeme_gui.h
 * @brief Embedded GUI & ILI9488 Display Driver for MiniScheme.
 *
 * Designed for ESP32 3.5-inch Desktop Information Display (320x480 / 480x320 RGB565)
 * with 8-bit Parallel 8080 bus, WS2812 RGB LED (IO21), Photoresistor (IO35),
 * Backlight PWM (IO22), User Button (IO18), capacitive touch simulation & hit-testing.
 */

#ifndef MINISCHEME_GUI_H
#define MINISCHEME_GUI_H

#include "minischeme.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Standard LCD Dimensions for 3.5-inch ILI9488 ESP32 Desktop Information Display (Default: 320x480 Portrait) */
#define MINI_GUI_DEFAULT_WIDTH  320
#define MINI_GUI_DEFAULT_HEIGHT 480
#define MINI_GUI_MAX_PIXELS     (480 * 480)

/* Color conversion: 24-bit RGB (8-8-8) to 16-bit RGB565 */
static inline uint16_t mini_gui_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/* Common RGB565 Predefined Colors */
#define MINI_COLOR_BLACK       0x0000
#define MINI_COLOR_WHITE       0xFFFF
#define MINI_COLOR_RED         0xF800
#define MINI_COLOR_GREEN       0x07E0
#define MINI_COLOR_BLUE        0x001F
#define MINI_COLOR_CYAN        0x07FF
#define MINI_COLOR_MAGENTA     0xF81F
#define MINI_COLOR_YELLOW      0xFFE0
#define MINI_COLOR_ORANGE      0xFD20
#define MINI_COLOR_GRAY        0x8410
#define MINI_COLOR_DARK_GRAY   0x31A6
#define MINI_COLOR_LIGHT_GRAY  0xC618
#define MINI_COLOR_NAVY        0x000F
#define MINI_COLOR_BG_DARK     0x0883 /* #0b1118 Dark Slate (app theme canvas) */
#define MINI_COLOR_CARD_BG     0x18E5 /* #181c2b Dark Navy Card */
#define MINI_COLOR_CARD_BORDER 0x296B /* #29304a Border */
#define MINI_COLOR_ACCENT_BLUE 0x3D3F /* #3b82f6 Accent Blue */

/* The pixel pipeline has three independent settings:
 *  - mode: where pixels are kept. BAND re-renders a few rows per slice,
 *    FULL keeps the whole frame, DIRECT keeps nothing (spans go straight
 *    to the panel through a MiniGuiDirectSink).
 *  - color bits: 16 stores RGB565; 8, 4, 2 and 1 store palette indices,
 *    so a band or frame costs bits/16 of its RGB565 size.
 *  - color scheme: the palette and how drawing colors map onto it. */
typedef enum {
    MINI_GUI_MODE_FULL   = 0,
    MINI_GUI_MODE_BAND   = 1,
    MINI_GUI_MODE_DIRECT = 2,
} MiniGuiMode;

/* Panel bus format (MINI_GUI_COLOR_4BPP is a legacy alias for 4 color bits) */
typedef enum {
    MINI_GUI_COLOR_4BPP   = 4,
    MINI_GUI_COLOR_RGB565 = 16,
    MINI_GUI_COLOR_RGB666 = 18,
} MiniGuiColorFormat;

typedef enum {
    MINI_GUI_SCHEME_COLOR = 0, /* full color; below 16 bits the nearest palette entry */
    MINI_GUI_SCHEME_GRAY,      /* gray levels by luminance */
    MINI_GUI_SCHEME_EINK,      /* black ink on white paper (dark UIs come out inverted) */
    MINI_GUI_SCHEME_GREEN,     /* phosphor green */
    MINI_GUI_SCHEME_AMBER,     /* amber CRT */
    MINI_GUI_SCHEME_CYAN,      /* cyan on navy */
    MINI_GUI_SCHEME_GAMEBOY,   /* four olive greens */
    MINI_GUI_SCHEME_INVERT,    /* every color inverted (RGB negative): light UI from a dark one */
    MINI_GUI_SCHEME_CUSTOM,    /* entries set with mini_gui_set_palette */
    MINI_GUI_SCHEME_COUNT
} MiniGuiScheme;

typedef struct {
    int width, height;
    MiniGuiMode mode;
    int band_lines;            /* BAND: rows per slice */
    int color_bits;            /* 16, 8, 4, 2 or 1 */
    MiniGuiScheme scheme;
    MiniGuiColorFormat bus;    /* panel bus: RGB565 or RGB666 */
} MiniGuiConfig;

/* Applies a display configuration. Returns false when the requested buffer
 * could not be allocated; FULL then falls back to BAND (check the mode). */
bool mini_gui_configure(const MiniGuiConfig *cfg);
void mini_gui_get_config(MiniGuiConfig *out);

void mini_gui_set_color_bits(int bits);
int  mini_gui_get_color_bits(void);
void mini_gui_set_scheme(MiniGuiScheme scheme);
MiniGuiScheme mini_gui_get_scheme(void);
const char *mini_gui_scheme_name(MiniGuiScheme scheme);
/* Scheme for a name ("color", "gray", "eink", ... and legacy preset names); -1 if unknown */
int  mini_gui_scheme_from_name(const char *name);

/* Rows handed to the panel: RGB565, or palette indices packed MSB-first */
typedef struct {
    const uint8_t *data;       /* first row */
    size_t stride;             /* bytes per row */
    int bits;                  /* 16, 8, 4, 2 or 1 */
    const uint16_t *palette;   /* RGB565 per index when bits < 16 */
} MiniGuiRows;

/* Expands count pixels of row `row`, starting at column x, to RGB565 */
void mini_gui_rows_to_rgb565(const MiniGuiRows *rows, int row, int x, int count, uint16_t *out);

/* DIRECT mode output: every drawn span becomes one fill on the panel */
typedef struct {
    void (*fill)(int x, int y, int w, int h, uint16_t rgb565, void *userdata);
} MiniGuiDirectSink;

/* Core Framebuffer Management */
void mini_gui_init(int width, int height);
void mini_gui_init_config(int width, int height, MiniGuiMode mode, int band_lines, MiniGuiColorFormat color_fmt);
MiniGuiMode mini_gui_get_mode(void);
int  mini_gui_get_band_lines(void);
MiniGuiColorFormat mini_gui_get_color_format(void);
uint16_t *mini_gui_get_band_buffer(void); /* RGB565 band slice */
void mini_gui_set_resolution(int width, int height);
void mini_gui_clear(uint16_t color);
void mini_gui_flush(void);
uint16_t *mini_gui_get_vfb(void);
int  mini_gui_get_width(void);
int  mini_gui_get_height(void);
bool mini_gui_is_dirty(void);
void mini_gui_clear_dirty(void);

/* Framebuffer Management, Monochrome & 2^N Reduced Color Depth (Palette) */
#define MINI_GUI_BPP_1   1
#define MINI_GUI_BPP_2   2
#define MINI_GUI_BPP_4   4
#define MINI_GUI_BPP_8   8
#define MINI_GUI_BPP_16  16

void     mini_gui_set_fb_enabled(bool enabled);
bool     mini_gui_is_fb_enabled(void);
void     mini_gui_set_color_depth(int bpp);
int      mini_gui_get_color_depth(void);
size_t   mini_gui_get_fb_ram(void);

void     mini_gui_set_mono_colors(uint16_t fg, uint16_t bg);
void     mini_gui_get_mono_colors(uint16_t *out_fg, uint16_t *out_bg);

void     mini_gui_set_palette(int index, uint16_t rgb565);
uint16_t mini_gui_get_palette(int index);
void     mini_gui_set_palette_preset(int preset_id); /* legacy ids 0..7, sets the scheme */
void     mini_gui_requantize(void);                  /* redraws the screen */

/* Dirty Rectangle Tracking & Tile Grid for Partial Refresh */
#define MINI_GUI_TILE_SIZE       32
#define MINI_GUI_MAX_DIRTY_RECTS 8

void mini_gui_mark_dirty_rect(int x, int y, int w, int h);
bool mini_gui_get_dirty_rect(int *out_x, int *out_y, int *out_w, int *out_h);
int  mini_gui_get_dirty_rect_count(void);
bool mini_gui_get_dirty_rect_at(int idx, int *out_x, int *out_y, int *out_w, int *out_h);

/* Clipping Region Management */
void mini_gui_set_clip(int x, int y, int w, int h);
void mini_gui_reset_clip(void);
void mini_gui_get_clip(int *out_x, int *out_y, int *out_w, int *out_h);

/* Sub-Framebuffer & Offscreen Component Buffers */
typedef struct MiniSubBuffer {
    int width;
    int height;
    uint16_t *pixels;
} MiniSubBuffer;

MiniSubBuffer *mini_gui_create_buffer(int w, int h);
void mini_gui_destroy_buffer(MiniSubBuffer *buf);
void mini_gui_set_target(MiniSubBuffer *buf);
void mini_gui_reset_target(void);
void mini_gui_blit_buffer(int dest_x, int dest_y, const MiniSubBuffer *buf);

/* Scanline / Banding Slicing Render Mode (Ultra-low RAM) */
typedef void (*MiniBandDrawFn)(int band_idx, int band_y, int band_h, void *userdata);
void mini_gui_render_bands(int band_h, MiniBandDrawFn fn, void *userdata);
/* The same for screen rows [y0, y1) only (the rest of the panel untouched) */
void mini_gui_render_band_rows(int y0, int y1, int band_h, MiniBandDrawFn fn, void *userdata);
void mini_gui_set_band_mode(bool enabled, int band_y, int band_h, uint16_t *band_pixels, int band_stride);
bool mini_gui_is_band_mode(void);

/* Retained-mode band pipeline shared by firmware and simulators.
 * Scheme requests redraws with (gui-invalidate ...); the host drains the
 * pending row range and re-renders it band by band through the render
 * procedure: the global `gui-render-proc`. */
typedef void (*MiniBandEmitFn)(const MiniGuiRows *rows, int y, int h, int width, void *userdata);

/* Where finished rows (BAND, FULL) and direct spans go. Without them, host
 * builds draw into the RGB565 VFB (mini_gui_get_vfb). */
void mini_gui_set_output(MiniBandEmitFn emit, const MiniGuiDirectSink *sink, void *userdata);
/* A panel that takes rectangles: rows w pixels wide (stride = w's bytes) for
 * screen [x, x + w) x [y, y + h). With it, a moving text box is redrawn and
 * sent on its own in BAND mode (userdata as mini_gui_set_output's). Without
 * it the box rows are re-rendered full width; the simulator's VFB takes
 * rectangles by itself. */
typedef void (*MiniRectEmitFn)(const MiniGuiRows *rows, int x, int y, int w, int h, void *userdata);
void mini_gui_set_rect_output(MiniRectEmitFn emit);

/* Renders every pending invalidation in the current mode and, in FULL mode,
 * flushes rows drawn outside a render. Returns the render errors. */
int  mini_gui_service(MiniVM *vm, int *rows_out);
void mini_gui_invalidate(int y, int h);
/* Pops the topmost pending row span; loop until it returns false */
bool mini_gui_take_invalidated(int *out_y0, int *out_y1);
/* Renders rows [y0, y1) in the current mode (BAND slices, FULL frame or
 * DIRECT to the panel). emit == NULL uses the output set with
 * mini_gui_set_output, else the RGB565 VFB on host builds.
 * Returns the number of render-procedure calls that raised an error. */
int  mini_gui_band_render(MiniVM *vm, int y0, int y1, MiniBandEmitFn emit, void *userdata);
const char *mini_gui_band_last_error(void);
/* Rows [y0, y1) as currently shown, delivered through emit without touching
 * the panel (screen mirroring): BAND re-rasterizes whole bands, FULL reads
 * the frame. Returns false in DIRECT mode, which keeps no pixels. */
bool mini_gui_capture(MiniVM *vm, int y0, int y1, MiniBandEmitFn emit, void *userdata);
/* Commands recorded by the last band render (-1 = fell back to per-band Scheme) */
int  mini_gui_display_list_size(void);
/* KEY 18 hold feedback: up to three bars in the bottom-right corner over the
 * app (pale green filling, red once armed): the long press at the bottom,
 * the hardware pages (5 s) above it, the system settings (10 s) on top.
 * permille 0..1000; only the corner rows are redrawn. */
#define MINI_HOLD_VISIBLE        1
#define MINI_HOLD_LONG_ARMED     2
#define MINI_HOLD_INFO_VISIBLE   4     /* the top bar: the hold (hardware pages) */
#define MINI_HOLD_INFO_ARMED     8
#define MINI_HOLD_MID_VISIBLE    16    /* the middle bar: the mid press */
#define MINI_HOLD_MID_ARMED      32
void mini_gui_set_hold_overlay(int flags, int long_permille, int mid_permille, int hold_permille);
/* Current MINI_HOLD_* flags and filled widths (px) */
int  mini_gui_hold_overlay(int *long_px, int *mid_px, int *top_px);
/* Commands the display list holds now: 128 static, more while grown from
 * the heap for a busy page (up to 512) */
int  mini_gui_display_list_capacity(void);
/* Heap bytes the grown display list uses (0 on the static arrays) */
size_t mini_gui_display_list_heap_bytes(void);

/* Drawing Primitives */
void mini_gui_pixel(int x, int y, uint16_t color);
uint16_t mini_gui_get_pixel(int x, int y);
void mini_gui_line(int x0, int y0, int x1, int y1, uint16_t color);
/* Polyline of n points across w; ys[i] = row offset inside [y, y + h) */
void mini_gui_chart(int x, int y, int w, int h, const uint8_t *ys, int n, uint16_t color);
/* A 1-bit bitmap of w x h dots, rows of (w + 7) / 8 bytes, the first dot of a
 * byte its high bit; each dot scale x scale pixels. Set dots fg, clear ones
 * bg, or left as they are when bg < 0. One display-list command. */
void mini_gui_bitmap(int x, int y, int w, int h, const uint8_t *bits, int scale, uint16_t fg, int bg);
/* An image file (minischeme_image: a baseline JPEG or a MANI animation) at
 * x y, path as resolved for the file system. frame >= 0: that frame; -1: an
 * animation plays (frame clock, its delays at speed percent). One
 * display-list command; a band decodes only its rows. */
void mini_gui_image(int x, int y, const char *path, int frame, int speed);
/* Closes the image kept open (call when image files change) */
void mini_gui_image_forget(void);
/* Ring arc: start angle 0 = 3 o'clock, sweeping clockwise */
void mini_gui_arc(int cx, int cy, int r, int thick, int start_deg, int sweep_deg, uint16_t color);
void mini_gui_rect(int x, int y, int w, int h, uint16_t color, bool filled);
void mini_gui_round_rect(int x, int y, int w, int h, int r, uint16_t color, bool filled);
void mini_gui_card(int x, int y, int w, int h, int r, uint16_t bg_col, uint16_t border_col);
void mini_gui_circle(int cx, int cy, int r, uint16_t color, bool filled);
void mini_gui_text(int x, int y, const char *str, uint16_t color, int size);

/* Font Selection, Measurement & CSS-like Box Alignment */
#define MINI_FONT_SYSTEM_16 0
#define MINI_FONT_CLOCK_32  1

void mini_gui_set_font(int font_id);
int  mini_gui_get_font(void);
int  mini_gui_text_width(const char *str, int font_id);
void mini_gui_text_styled(int x, int y, const char *str, uint16_t color, int font_id);
void mini_gui_text_box(int x, int y, int w, int h, const char *str, uint16_t color, int font_id, int halign, int valign);

/* Text longer than its box scrolls in a loop at speed pixels per second:
 * MINI_GUI_SCROLL_LEFT runs one line right to left (marquee),
 * MINI_GUI_SCROLL_UP wraps the text to the box width and rolls it upward.
 * Text that fits is drawn still. Call it from the render procedure. */
#define MINI_GUI_SCROLL_LEFT 0
#define MINI_GUI_SCROLL_UP   1
/* One line (gui-text-fit) / wrapped text (gui-text-area, speed = line
 * height, 0 for the font's) that follow the marquee settings when too big */
#define MINI_GUI_SCROLL_FIT  2
#define MINI_GUI_SCROLL_AREA 3

/* Text too big for its box, config.ini [text]: one-line text (marquee) and
 * wrapped text (wrap_marquee) are set apart. NONE cuts it with an ellipsis;
 * PIXEL moves `step` pixels, CHAR `step` characters (one line), LINE `step`
 * lines (wrapped), PAGE shows the next box-full piece; fps_x10 refreshes per
 * 10 s (1..250, i.e. 0.1..25 a second) */
#define MINI_GUI_TEXT_LINE 0
#define MINI_GUI_TEXT_WRAP 1
#define MINI_GUI_MARQUEE_NONE  0
#define MINI_GUI_MARQUEE_PIXEL 1
#define MINI_GUI_MARQUEE_CHAR  2
#define MINI_GUI_MARQUEE_LINE  3
#define MINI_GUI_MARQUEE_PAGE  4
void mini_gui_set_marquee(int which, int mode, int fps_x10, int step);
void mini_gui_get_marquee(int which, int *mode, int *fps_x10, int *step);
const char *mini_gui_marquee_name(int mode);
int mini_gui_marquee_parse(int which, const char *name);  /* -1 when unknown or not for `which` */
/* How long the start stays after the marquee jumps back (ms, 0..10000; 1000) */
void mini_gui_set_marquee_hold(int which, int hold_ms);
int  mini_gui_get_marquee_hold(int which);
/* Text wrapped to width w (at spaces, else between CJK characters), as many
 * lines of line_h (0: font height + 2) as fit in h, the last cut one ending
 * in an ellipsis. Returns the lines drawn. */
int mini_gui_text_wrap(int x, int y, int w, int h, const char *str, uint16_t color, int font_id, int line_h);
/* The lines of str wrapped to w pixels as gui-text-wrap / gui-text-area lay
 * them out: byte offsets of each line's start and end (up to max); returns
 * the number of lines. A line may end at a space or next to a CJK
 * character; Latin words stay whole; no closing punctuation starts a line
 * and no opening punctuation ends one. */
int mini_gui_wrap_text(const char *str, int font_id, int w, int *starts, int *ends, int max);
void mini_gui_text_scroll(int x, int y, int w, int h, const char *str, uint16_t color,
                          int font_id, int speed, int dir);
/* Frame clock for scrolling text: call from the main loop with milliseconds.
 * Re-invalidates every moving text box at most every 40 ms; true if it did. */
bool mini_gui_animate(uint32_t now_ms);
/* Makes the next band render run the render procedure even if only scrolling
 * text moved (frames that only move text replay the recorded display list) */
void mini_gui_forget_recording(void);

/* Seven-segment digits ("0-9", "-", ":", "."): ghost segments in off_col,
 * lit segments in on_col. glow_col / hi_col add a halo and centre highlight;
 * pass on_col for both to get flat LCD segments. Returns the drawn width. */
int mini_gui_seg7(int x, int y, int w, int h, int t, const char *str,
                  uint16_t on_col, uint16_t off_col, uint16_t glow_col, uint16_t hi_col);
int mini_gui_seg7_width(int w, int t, const char *str);

/* Touch Screen Event Simulation & Driver */
void mini_gui_inject_touch(bool is_down, int x, int y);
bool mini_gui_get_touch(int *out_x, int *out_y);
bool mini_gui_is_touch_down(void);

/* Hardware Peripherals (WS2812, Light Sensor, Backlight, Button) */
void mini_ws2812_set(uint8_t r, uint8_t g, uint8_t b);
void mini_ws2812_get(uint8_t *r, uint8_t *g, uint8_t *b);
void mini_light_set(int val); // for simulator injection (0-4095)
int  mini_light_read(void);
void mini_backlight_set(uint8_t val); // 0-255 PWM
uint8_t mini_backlight_get(void);
/* Automatic backlight from the light sensor: on by default; (backlight-set)
 * turns it off, (backlight-auto #t) on. mini_backlight_auto_step moves the
 * brightness half way to the sensor's value (once a second); the hook hears
 * every change made from Scheme so the host can keep it. */
bool mini_backlight_auto(void);
void mini_backlight_auto_set(bool on);
void mini_backlight_set_params(int offset, int min_val, int max_val);
int  mini_backlight_get_offset(void);
int  mini_backlight_get_min(void);
int  mini_backlight_get_max(void);
int  mini_backlight_calc_dynamic(int light_raw, int min_val, int max_val, int offset);
void mini_backlight_auto_step(void);
void mini_backlight_set_hook(void (*hook)(bool auto_on, int level));
void mini_button_set(bool pressed); // for simulator injection
bool mini_button_read(void);

/* Register all GUI & Peripheral Scheme primitives in the given VM */
void mini_register_gui_lib(MiniVM *vm);
/* font-load, font-register!, font-unload, font-info, use-font (minischeme_fontreg.c);
 * registered by mini_register_gui_lib */
void mini_register_font_lib(MiniVM *vm);
/* Re-registers the fonts an app registered (after booting a FASL snapshot) */
void mini_font_restore(MiniVM *vm);

#ifdef __cplusplus
}
#endif

#endif /* MINISCHEME_GUI_H */
