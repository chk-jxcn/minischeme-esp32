/**
 * @file minischeme_gui.c
 * @brief Embedded GUI & Virtual LCD Display Driver for MiniScheme.
 */

#include "minischeme_image.h"
#include "minischeme_gui.h"
#include "minischeme_internal.h"
#include "minischeme_fonts.h"
#include "minischeme_ili9488.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#define GUI_MAX_PIXELS (480 * 480)

#if !defined(ESP_PLATFORM)
/* Host builds (WASM, PC tests): s_vfb mirrors the panel in RGB565 and the
 * pixel surface lives in one static pool (the WASM allocator never frees) */
static uint16_t s_vfb_static[GUI_MAX_PIXELS];
static uint16_t *s_vfb = s_vfb_static;
static uint8_t  s_surf_pool[GUI_MAX_PIXELS * 2];
#else
#include "esp_heap_caps.h"
static uint16_t *s_vfb = NULL;
#endif
static uint16_t *s_target_pixels = NULL; /* RGB565 sub-buffer while one is targeted */

static MiniGuiMode        s_gui_mode = MINI_GUI_MODE_DIRECT;
static int                s_gui_band_lines = 20;
static MiniGuiColorFormat s_gui_color_format = MINI_GUI_COLOR_RGB565;

static int      s_width = MINI_GUI_DEFAULT_WIDTH;
static int      s_height = MINI_GUI_DEFAULT_HEIGHT;
static int      s_target_width = MINI_GUI_DEFAULT_WIDTH;
static int      s_target_height = MINI_GUI_DEFAULT_HEIGHT;
static MiniSubBuffer *s_active_sub_buffer = NULL;

/* Effective clip used by rasterizers (user clip intersected with active band) */
static int s_clip_x0 = 0;
static int s_clip_y0 = 0;
static int s_clip_x1 = MINI_GUI_DEFAULT_WIDTH;
static int s_clip_y1 = MINI_GUI_DEFAULT_HEIGHT;

/* User clip in logical target coordinates (what gui-get-clip reports) */
static int s_uclip_x0 = 0;
static int s_uclip_y0 = 0;
static int s_uclip_x1 = MINI_GUI_DEFAULT_WIDTH;
static int s_uclip_y1 = MINI_GUI_DEFAULT_HEIGHT;

/* Dirty Rectangle Tracking & Tile Grid for Partial Refresh */
static bool s_dirty = false;
static int  s_dirty_x0 = MINI_GUI_DEFAULT_WIDTH;
static int  s_dirty_y0 = MINI_GUI_DEFAULT_HEIGHT;
static int  s_dirty_x1 = 0;
static int  s_dirty_y1 = 0;

#define MINI_GUI_MAX_TILES_X 16
#define MINI_GUI_MAX_TILES_Y 16
#define MINI_GUI_TOTAL_TILES (MINI_GUI_MAX_TILES_X * MINI_GUI_MAX_TILES_Y)
#define MINI_GUI_TILE_WORDS  (MINI_GUI_TOTAL_TILES / 32)
static uint32_t s_dirty_tiles[MINI_GUI_TILE_WORDS];

typedef struct MiniDirtyRect {
    int x, y, w, h;
} MiniDirtyRect;

static MiniDirtyRect s_coalesced_dirty_rects[MINI_GUI_MAX_DIRTY_RECTS];
static int           s_coalesced_dirty_count = 0;
static bool          s_coalesced_dirty_valid = false;


/* ------------------------------------------------------------------------- */
/* Display list: the render procedure runs once per redraw, drawing calls are */
/* recorded here and replayed in C for every band slice.                     */
/* ------------------------------------------------------------------------- */
enum {
    GUI_OP_CLEAR = 1, GUI_OP_PIXEL, GUI_OP_LINE, GUI_OP_RECT, GUI_OP_RRECT, GUI_OP_CARD,
    GUI_OP_CIRCLE, GUI_OP_TEXT, GUI_OP_TEXT_STYLED, GUI_OP_TEXT_BOX, GUI_OP_SEG7,
    GUI_OP_SET_CLIP, GUI_OP_RESET_CLIP, GUI_OP_CHART, GUI_OP_ARC, GUI_OP_TEXT_SCROLL, GUI_OP_BITMAP,
    GUI_OP_IMAGE
};

typedef struct {
    uint8_t  op;
    uint8_t  b;                 /* small flag / packed alignment argument */
    int16_t  x, y, w, h, p;     /* geometry; p = radius, font, size or thickness */
    uint16_t c1, c2, c3, c4;    /* colours */
    uint16_t str;               /* offset into s_dl_strings, 0xFFFF = none */
} GuiCmd;

/* A recorded command as the list stores it: six 32-bit words, so the list
 * can live in word-only memory (IRAM on the ESP32, mini_word_alloc).
 *   w0 op | b << 8 | str << 16   w1 x | y << 16   w2 w | h << 16
 *   w3 p | c1 << 16              w4 c2 | c3 << 16   w5 c4 */
typedef struct {
    uint32_t w[6];
} GuiCmdW;

#define LO16(v) ((uint32_t)(uint16_t)(v))

/* Every access is a whole volatile word: without volatile the compiler may
 * read just the byte or halfword a field needs (l8ui, l16ui), which faults
 * in IRAM (LoadStoreError) */
static void dl_store(GuiCmdW *d, const GuiCmd *c) {
    volatile uint32_t *w = d->w;
    w[0] = (uint32_t)c->op | (uint32_t)c->b << 8 | LO16(c->str) << 16;
    w[1] = LO16(c->x) | LO16(c->y) << 16;
    w[2] = LO16(c->w) | LO16(c->h) << 16;
    w[3] = LO16(c->p) | LO16(c->c1) << 16;
    w[4] = LO16(c->c2) | LO16(c->c3) << 16;
    w[5] = LO16(c->c4);
}

static void dl_load(const GuiCmdW *d, GuiCmd *c) {
    const volatile uint32_t *w = d->w;
    uint32_t w0 = w[0], w1 = w[1], w2 = w[2], w3 = w[3], w4 = w[4], w5 = w[5];
    c->op = (uint8_t)w0;
    c->b = (uint8_t)(w0 >> 8);
    c->str = (uint16_t)(w0 >> 16);
    c->x = (int16_t)(uint16_t)w1;
    c->y = (int16_t)(uint16_t)(w1 >> 16);
    c->w = (int16_t)(uint16_t)w2;
    c->h = (int16_t)(uint16_t)(w2 >> 16);
    c->p = (int16_t)(uint16_t)w3;
    c->c1 = (uint16_t)(w3 >> 16);
    c->c2 = (uint16_t)w4;
    c->c3 = (uint16_t)(w4 >> 16);
    c->c4 = (uint16_t)w5;
}

/* The list's base arrays are sized for ordinary pages (the busiest app pages
 * need 70..105 commands): the strings static, the commands in word memory
 * (IRAM on the ESP32), allocated at the first recording. A frame that needs
 * more grows them in steps, up to GUI_DL_MAX_*; only past that, or without
 * memory, does Scheme run once per band. After GUI_DL_SHRINK_AFTER
 * recordings that fit the base arrays again, the grown copies are freed. */
#define GUI_DL_BASE_CMDS     128
#define GUI_DL_BASE_STRINGS  1536
#define GUI_DL_GROW_CMDS     64
#define GUI_DL_GROW_STRINGS  1024
#define GUI_DL_MAX_CMDS      512
#define GUI_DL_MAX_STRINGS   8192  /* below the 0xFFFF "no string" offset */
#define GUI_DL_SHRINK_AFTER  8
#if defined(__wasm__)
#define GUI_DL_SHRINK 0            /* the WebREPL's malloc never frees: keep what was grown */
#else
#define GUI_DL_SHRINK 1
#endif
static GuiCmdW *s_dl_base_cmds = NULL;   /* [GUI_DL_BASE_CMDS], word memory */
static char    s_dl_base_strings[GUI_DL_BASE_STRINGS];
static GuiCmdW *s_dl_cmds = NULL;
static char   *s_dl_strings = s_dl_base_strings;
static int     s_dl_cap = GUI_DL_BASE_CMDS;
static size_t  s_dl_str_cap = GUI_DL_BASE_STRINGS;
static int     s_dl_small_runs = 0;    /* recordings in a row that fit the static arrays */
static int     s_dl_count = 0;
static int     s_dl_str_used = 0;
static bool    s_rec_on = false;       /* drawing calls record instead of raster */
static bool    s_rec_overflow = false; /* list full or unrecordable call seen */
/* The list still is what the render procedure draws for rows [s_dl_y0,
 * s_dl_y1): no gui-invalidate since it was recorded. Frames that only move
 * scrolling text replay it without running Scheme. */
static bool    s_dl_fresh = false;
static int     s_dl_y0 = 0;
static int     s_dl_y1 = 0;

/* Scrolling text boxes (mini_gui_text_scroll): the screen rectangle to redraw
 * while they move, how often, and the frame clock their offset comes from.
 * A box that is due is redrawn on its own by mini_gui_service (one narrow
 * rectangle, one transfer) when the band mode allows it. */
#define GUI_ANIM_MAX      16
#define GUI_ANIM_FRAME_MS 40  /* gui-text-scroll: 25 frames per second */
typedef struct {
    int16_t  x0, y0, x1, y1;
    uint32_t period;   /* ms between redraws */
    uint32_t last;
    bool     due;
} GuiAnim;
static GuiAnim  s_anim[GUI_ANIM_MAX];
static int      s_anim_n = 0;
static uint32_t s_now_ms = 0;    /* clock passed to mini_gui_animate */
static uint32_t s_frame_ms = 0;  /* one value for every band of a frame */
static void gui_anim_forget(int y0, int y1);
static void gui_anim_add(int x0, int y0, int x1, int y1, uint32_t period_ms);
static uint32_t gui_epoch_start(const char *str, int x, int y, int w, int h, int dir);
static void gui_epoch_sweep(void);
static uint16_t utf8_next(const unsigned char **pp);

/* When each marquee box appeared: its cycle (the hold, then the steps)
 * counts from then, not from the clock, so a page that has just come up
 * starts still. A box keeps its time while it stays drawn (the same text in
 * the same place); one that is no longer drawn when its rows are rendered
 * again is dropped, so it starts over when it comes back. */
#define GUI_EPOCH_MAX 24
typedef struct {
    uint32_t key;       /* text and box; 0 = free */
    uint32_t start;     /* s_now_ms when it was first drawn */
    uint32_t seen;      /* last drawn (the oldest goes when the table is full) */
    int16_t  y0, y1;
    bool     stale;     /* its rows are being rendered again: still drawn? */
} GuiEpoch;
static GuiEpoch s_epoch[GUI_EPOCH_MAX];
static int gui_char_width(const MiniFont *font, uint16_t cp);

/* Text too big for its box, config.ini [text]: one-line text (gui-text-fit)
 * and wrapped text (gui-text-area) each have a mode, refreshes per second
 * (in tenths) and a step */
typedef struct {
    int mode;      /* MINI_GUI_MARQUEE_* */
    int fps_x10;   /* 1..250 */
    int step;      /* pixels, characters or lines per refresh */
    int hold_ms;   /* the start stays this long (0..10000) */
} GuiMarquee;
static GuiMarquee s_marquee[2] = {
    { MINI_GUI_MARQUEE_PIXEL, 200, 1, 1000 },  /* one line: 20 px a second, 1 px at a time */
    { MINI_GUI_MARQUEE_LINE,    5, 1, 1000 },  /* wrapped: up one line every 2 s */
};

/* Room for one more command, growing the list from the heap if needed */
static bool gui_dl_room_cmd(void) {
    if (!s_dl_base_cmds) {
        s_dl_base_cmds = (GuiCmdW *)mini_word_alloc((size_t)GUI_DL_BASE_CMDS * sizeof(GuiCmdW));
        if (!s_dl_base_cmds) return false;
        s_dl_cmds = s_dl_base_cmds;
        s_dl_cap = GUI_DL_BASE_CMDS;
    }
    if (s_dl_count < s_dl_cap) return true;
    if (s_dl_cap >= GUI_DL_MAX_CMDS) return false;
    int cap = s_dl_cap + GUI_DL_GROW_CMDS > GUI_DL_MAX_CMDS ? GUI_DL_MAX_CMDS : s_dl_cap + GUI_DL_GROW_CMDS;
    GuiCmdW *p = (GuiCmdW *)mini_word_alloc((size_t)cap * sizeof(GuiCmdW));
    if (!p) return false;
    mini_words_copy(p, s_dl_cmds, (size_t)s_dl_count * sizeof(GuiCmdW));
    if (s_dl_cmds != s_dl_base_cmds) free(s_dl_cmds);
    s_dl_cmds = p;
    s_dl_cap = cap;
    return true;
}

/* Room for n more string-pool bytes, growing the pool from the heap if needed */
static bool gui_dl_room_str(size_t n) {
    size_t need = (size_t)s_dl_str_used + n;
    if (need <= s_dl_str_cap) return true;
    size_t cap = s_dl_str_cap;
    while (cap < need) cap += GUI_DL_GROW_STRINGS;
    if (cap > GUI_DL_MAX_STRINGS) return false;
    char *p = (char *)malloc(cap);
    if (!p) return false;
    memcpy(p, s_dl_strings, (size_t)s_dl_str_used);
    if (s_dl_strings != s_dl_base_strings) free(s_dl_strings);
    s_dl_strings = p;
    s_dl_str_cap = cap;
    return true;
}

/* Before a recording: back to the static arrays once the last recordings
 * all fit them (the list is empty here, nothing to copy) */
static void gui_dl_maybe_shrink(void) {
    if (!GUI_DL_SHRINK || s_dl_small_runs < GUI_DL_SHRINK_AFTER) return;
    if (s_dl_cmds != s_dl_base_cmds) {
        free(s_dl_cmds);
        s_dl_cmds = s_dl_base_cmds;
        s_dl_cap = GUI_DL_BASE_CMDS;
    }
    if (s_dl_strings != s_dl_base_strings) {
        free(s_dl_strings);
        s_dl_strings = s_dl_base_strings;
        s_dl_str_cap = GUI_DL_BASE_STRINGS;
    }
}

/* Appends one command; returns true so draw functions can simply return */
static bool gui_rec(uint8_t op, int x, int y, int w, int h, int p, uint8_t b,
                    uint16_t c1, uint16_t c2, uint16_t c3, uint16_t c4, const char *str) {
    if (!gui_dl_room_cmd()) {
        s_rec_overflow = true;
        return true;
    }
    GuiCmd cmd, *c = &cmd;
    c->str = 0xFFFF;
    if (str) {
        size_t n = strlen(str) + 1;
        if (!gui_dl_room_str(n)) {
            s_rec_overflow = true;
            return true;
        }
        memcpy(&s_dl_strings[s_dl_str_used], str, n);
        c->str = (uint16_t)s_dl_str_used;
        s_dl_str_used += (int)n;
    }
    c->op = op;
    c->b = b;
    c->x = (int16_t)x; c->y = (int16_t)y; c->w = (int16_t)w; c->h = (int16_t)h; c->p = (int16_t)p;
    c->c1 = c1; c->c2 = c2; c->c3 = c3; c->c4 = c4;
    dl_store(&s_dl_cmds[s_dl_count++], c);
    return true;
}

/* Records a command whose payload is n raw bytes (chart points) */
static bool gui_rec_bytes(uint8_t op, int x, int y, int w, int h, int n, uint16_t c1, const uint8_t *data) {
    if (!gui_dl_room_cmd() || !gui_dl_room_str((size_t)n)) {
        s_rec_overflow = true;
        return true;
    }
    GuiCmd cmd, *c = &cmd;
    memcpy(&s_dl_strings[s_dl_str_used], data, (size_t)n);
    c->str = (uint16_t)s_dl_str_used;
    s_dl_str_used += n;
    c->op = op;
    c->b = 0;
    c->x = (int16_t)x; c->y = (int16_t)y; c->w = (int16_t)w; c->h = (int16_t)h; c->p = (int16_t)n;
    c->c1 = c1; c->c2 = c->c3 = c->c4 = 0;
    dl_store(&s_dl_cmds[s_dl_count++], c);
    return true;
}

/* Records a bitmap: its rows go to the string pool (they may hold NUL bytes) */
static void gui_rec_bitmap(int x, int y, int w, int h, const uint8_t *bits, int scale, uint16_t fg, int bg) {
    size_t n = (size_t)h * (size_t)((w + 7) / 8);
    if (!gui_dl_room_cmd() || !gui_dl_room_str(n)) {
        s_rec_overflow = true;
        return;
    }
    GuiCmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    memcpy(&s_dl_strings[s_dl_str_used], bits, n);
    cmd.str = (uint16_t)s_dl_str_used;
    s_dl_str_used += (int)n;
    cmd.op = GUI_OP_BITMAP;
    cmd.b = bg >= 0 ? 1 : 0;
    cmd.x = (int16_t)x; cmd.y = (int16_t)y; cmd.w = (int16_t)w; cmd.h = (int16_t)h; cmd.p = (int16_t)scale;
    cmd.c1 = fg;
    cmd.c2 = bg >= 0 ? (uint16_t)bg : 0;
    dl_store(&s_dl_cmds[s_dl_count++], &cmd);
}

#define GUI_REC(op, x, y, w, h, p, b, c1, c2, c3, c4, str) \
    do { if (s_rec_on) { gui_rec((op), (x), (y), (w), (h), (p), (b), (c1), (c2), (c3), (c4), (str)); return; } } while (0)

/* Rows [y, y+h) entirely outside the effective clip: skip the whole primitive
 * (every band replays every command, so this is the common case) */
#define GUI_ROWS_HIDDEN(y, h) ((y) >= s_clip_y1 || (y) + (h) <= s_clip_y0)

/* Sub-buffer table */
#define MAX_SUB_BUFFERS 16
static MiniSubBuffer *s_sub_buffers[MAX_SUB_BUFFERS] = {NULL};

static bool s_touch_down = false;
static int  s_touch_x = 0;
static int  s_touch_y = 0;

/* Hardware Peripherals State (WS2812, Light Sensor, Backlight, Button) */
static uint8_t s_ws2812_r = 0;
static uint8_t s_ws2812_g = 0;
static uint8_t s_ws2812_b = 0;
static int     s_light_val = 2048; /* 0..4095 (IO35 / ADC1_CH7) */
static uint8_t s_backlight_val = 255; /* 0..255 (IO22 PWM) */
static bool    s_button_pressed = false; /* GPIO 18 (active-high) */

/* Standard 8x8 font bitmap table for ASCII 32..126 + degree symbol */
#include "scratch/font_data.inc"
/* Compact 16x16 CJK Dot-Matrix Font Table */
#include "scratch/cjk_font16.inc"

/* ========================================================================= */
/* Pixel Pipeline: buffer mode x color bits x color scheme                   */
/* ========================================================================= */
/* Drawing quantizes each color once into "ink" (RGB565 at 16 bits, else a
 * palette index), writes spans of ink into the surface (BAND rows or the
 * FULL frame) or sends them to the panel (DIRECT), and the palette expands
 * ink back to RGB565 when rows go out to the panel. */

#define GUI_SURF_CHUNKS    16
#define GUI_SURF_CHUNK_MAX (32 * 1024) /* full-frame chunks fit fragmented DRAM */

typedef struct {
    uint8_t *chunk[GUI_SURF_CHUNKS];
    int      nchunks;
    int      chunk_rows;  /* rows per chunk */
    int      rows;        /* rows held */
    int      y0;          /* screen row of row 0 (moves per band) */
    int      x0, x1;      /* screen columns held: x1 == 0 means the whole width
                           * (a box redrawn on its own holds [x0, x1)) */
    size_t   stride;      /* bytes per row */
    size_t   capacity;    /* bytes allocated (one chunk in BAND mode) */
} GuiSurface;

static GuiSurface    s_surf;
static int           s_bits = 16;
static MiniGuiScheme s_scheme = MINI_GUI_SCHEME_COLOR;
static uint16_t      s_palette[256]; /* RGB565 per ink below 16 bits; tint ramp at 16 */
static int           s_pal_n = 0;    /* entries in use; 0 = RGB565 passes through */
static uint16_t     *s_custom = NULL; /* user palette (mini_gui_set_palette), allocated on first use */

/* Render window: drawing is clipped to rows [s_win_y0, s_win_y1) */
static int s_win_x0 = 0, s_win_x1 = 1 << 14; /* columns of the drawing window */
static int s_win_y0 = 0;
static int s_win_y1 = MINI_GUI_DEFAULT_HEIGHT;

/* Panel output: finished rows (BAND, FULL) and direct spans */
static MiniBandEmitFn           s_out_emit = NULL;
static MiniRectEmitFn s_out_rect = NULL; /* the panel takes rectangles */
static const MiniGuiDirectSink *s_out_sink = NULL;
static void                    *s_out_ud = NULL;

/* FULL mode: rows drawn outside a render, flushed by mini_gui_service */
static int  s_flush_y0 = 0;
static int  s_flush_y1 = 0;
static bool s_in_band_render = false;

/* 16-color dashboard palette (color scheme, 4 bits) */
static const uint16_t s_dashboard_16[16] = {
    0x0000, /* 0: Black #000000 */
    0x10C3, /* 1: Deep Slate Theme BG #101820 */
    0x18E5, /* 2: Card Slate BG #181C2B */
    0x296B, /* 3: Card Border #29304A */
    0x3D3F, /* 4: Accent Blue #3B82F6 */
    0x07E0, /* 5: Emerald Green #00FF00 */
    0xF800, /* 6: Coral Red #FF0000 */
    0xFFE0, /* 7: Bright Yellow #FFFF00 */
    0xFD20, /* 8: Amber Orange #FFA500 */
    0x07FF, /* 9: Cyan Sky #00FFFF */
    0xF81F, /* 10: Magenta #FF00FF */
    0x8410, /* 11: Gray #808080 */
    0xC618, /* 12: Light Gray #C0C0C0 */
    0x31A6, /* 13: Dark Gray #303430 */
    0x000F, /* 14: Deep Navy #000080 */
    0xFFFF  /* 15: Pure White #FFFFFF */
};

static const char *const s_scheme_names[MINI_GUI_SCHEME_COUNT] = {
    "color", "gray", "eink", "green", "amber", "cyan", "gameboy", "invert", "custom"
};

/* Gradient stops of the ramp schemes, darkest input first; 0 for the others */
static int scheme_stops(MiniGuiScheme scheme, uint16_t *stops) {
    switch (scheme) {
        case MINI_GUI_SCHEME_GRAY:    stops[0] = 0x0000; stops[1] = 0xFFFF; return 2;
        case MINI_GUI_SCHEME_EINK:    stops[0] = 0xFFFF; stops[1] = 0x0000; return 2;
        case MINI_GUI_SCHEME_GREEN:   stops[0] = 0x0100; stops[1] = 0x07E0; return 2;
        case MINI_GUI_SCHEME_AMBER:   stops[0] = 0x1880; stops[1] = 0xFD20; return 2;
        case MINI_GUI_SCHEME_CYAN:    stops[0] = 0x000F; stops[1] = 0x07FF; return 2;
        case MINI_GUI_SCHEME_GAMEBOY:
            stops[0] = 0x0984; stops[1] = 0x2A85; stops[2] = 0x7509; stops[3] = 0x964C;
            return 4;
        default: return 0;
    }
}

/* Point num/den along a multi-stop RGB565 gradient */
static uint16_t ramp_color(const uint16_t *stops, int nstops, int num, int den) {
    if (den <= 0) return stops[0];
    int seg_num = num * (nstops - 1);
    int seg = seg_num / den;
    if (seg >= nstops - 1) return stops[nstops - 1];
    int t = seg_num - seg * den; /* 0..den-1 within the segment */
    uint16_t a = stops[seg], b = stops[seg + 1];
    int r = ((a >> 11) * (den - t) + (b >> 11) * t) / den;
    int g = (((a >> 5) & 63) * (den - t) + ((b >> 5) & 63) * t) / den;
    int bl = ((a & 31) * (den - t) + (b & 31) * t) / den;
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

/* Perceptual color distance: R*2, G*4, B*1 */
static inline int color_distance_sq(uint16_t c1, uint16_t c2) {
    if (c1 == c2) return 0;
    int dr = ((c1 >> 11) & 0x1F) - ((c2 >> 11) & 0x1F);
    int dg = (((c1 >> 5) & 0x3F) - ((c2 >> 5) & 0x3F)) >> 1;
    int db = (c1 & 0x1F) - (c2 & 0x1F);
    return dr * dr * 2 + dg * dg * 4 + db * db;
}

/* Perceptual luminance 0..255 */
static inline int color_luminance(uint16_t c) {
    int r = (c >> 11) & 0x1F;
    int g = (c >> 5) & 0x3F;
    int b = c & 0x1F;
    return (r * 8 * 77 + g * 4 * 150 + b * 8 * 29) >> 8;
}

/* Small direct-mapped cache: draws reuse few colors, a full search is rare */
#define GUI_INK_CACHE 64
static uint16_t s_cache_key[GUI_INK_CACHE];
static uint16_t s_cache_ink[GUI_INK_CACHE];
static uint8_t  s_cache_ok[GUI_INK_CACHE];

static void ink_cache_reset(void) {
    memset(s_cache_ok, 0, sizeof(s_cache_ok));
}

/* Luminance levels above the canvas color. With few levels, near-canvas
 * fills (header strips, unlit segment ghosts) stay background so text on
 * them and lit segments remain readable, while borders and text (well
 * above the canvas) always get a visible level. */
static int color_level(uint16_t color, int n) {
    int y = color_luminance(color);
    int black = color_luminance(MINI_COLOR_BG_DARK) + (n <= 4 ? 24 : 4);
    if (y < black) return 0;
    if (n <= 2) return n - 1;
    int span = 255 - black;
    if (span < 1) span = 1;
    int lvl = 1 + ((y - black) * (n - 2) + span / 2) / span; /* rounded: near-white reaches the top */
    return lvl > n - 1 ? n - 1 : lvl;
}

/* Nearest palette entry by perceptual distance; only entry 0 (black) is
 * kept away from anything visibly brighter, so lines never vanish */
static int color_nearest(uint16_t color, int n) {
    int y_in = color_luminance(color);
    int best_dist = 0x7FFFFFFF, best = 0;
    for (int i = 0; i < n; i++) {
        if (s_palette[i] == color) return i;
        int dist = color_distance_sq(color, s_palette[i]);
        if (i == 0 && y_in > 8) dist += 30000;
        if (dist < best_dist) {
            best_dist = dist;
            best = i;
        }
    }
    return best;
}

/* Ramps and every scheme at 1 or 2 bits map by luminance level */
static bool scheme_by_level(void) {
    uint16_t stops[4];
    return scheme_stops(s_scheme, stops) > 0 || s_bits <= 2;
}

/* What the surface stores for a drawing color */
static uint16_t gui_ink(uint16_t color) {
    /* invert: the negative of the color, then mapped as the color scheme */
    uint16_t c = s_scheme == MINI_GUI_SCHEME_INVERT ? (uint16_t)(color ^ 0xFFFF) : color;
    if (s_pal_n == 0) return c;
    unsigned slot = ((unsigned)color * 0x9E37u >> 10) & (GUI_INK_CACHE - 1);
    if (s_cache_ok[slot] && s_cache_key[slot] == color) return s_cache_ink[slot];
    int idx = scheme_by_level() ? color_level(c, s_pal_n) : color_nearest(c, s_pal_n);
    uint16_t ink = (s_bits == 16) ? s_palette[idx] : (uint16_t)idx;
    s_cache_key[slot] = color;
    s_cache_ink[slot] = ink;
    s_cache_ok[slot] = 1;
    return ink;
}

/* The RGB565 color the panel shows for a drawing color */
static uint16_t gui_panel_color(uint16_t color) {
    uint16_t ink = gui_ink(color);
    return s_bits < 16 ? s_palette[ink] : ink;
}

/* The custom palette, allocated on first use (the dashboard colors first);
 * NULL when memory is short */
static uint16_t *custom_pal(void) {
    if (!s_custom) {
        s_custom = (uint16_t *)malloc(256 * sizeof(uint16_t));
        if (s_custom) {
            for (int i = 0; i < 256; i++) s_custom[i] = i < 16 ? s_dashboard_16[i] : 0;
        }
    }
    return s_custom;
}

static void gui_build_palette(void) {
    uint16_t stops[4];
    int nstops = scheme_stops(s_scheme, stops);
    int n = s_bits < 16 ? 1 << s_bits : 0;
    if ((s_scheme == MINI_GUI_SCHEME_COLOR || s_scheme == MINI_GUI_SCHEME_INVERT) && s_bits <= 2) {
        /* Two or four levels from the canvas color up to white */
        stops[0] = MINI_COLOR_BG_DARK;
        stops[1] = MINI_COLOR_WHITE;
        nstops = 2;
    }
    if (nstops > 0) {
        if (n == 0) n = 64; /* 16 bits: a 64-level tint */
        for (int i = 0; i < n; i++) s_palette[i] = ramp_color(stops, nstops, i, n - 1);
    } else if (s_scheme == MINI_GUI_SCHEME_CUSTOM) {
        const uint16_t *cp = custom_pal();
        for (int i = 0; i < n; i++) s_palette[i] = cp ? cp[i] : (i < 16 ? s_dashboard_16[i] : 0);
    } else if (n == 256) {
        static const uint8_t levels[6] = { 0, 51, 102, 153, 204, 255 };
        int idx = 0;
        for (int i = 0; i < 16; i++) s_palette[idx++] = s_dashboard_16[i];
        for (int r = 0; r < 6; r++)
            for (int g = 0; g < 6; g++)
                for (int b = 0; b < 6; b++) s_palette[idx++] = mini_gui_rgb(levels[r], levels[g], levels[b]);
        for (int i = 0; i < 24; i++) {
            uint8_t gray = (uint8_t)(i * 255 / 23);
            s_palette[idx++] = mini_gui_rgb(gray, gray, gray);
        }
    } else if (n == 16) {
        for (int i = 0; i < 16; i++) s_palette[i] = s_dashboard_16[i];
    }
    s_pal_n = n;
    ink_cache_reset();
}

const char *mini_gui_scheme_name(MiniGuiScheme scheme) {
    return (scheme >= 0 && scheme < MINI_GUI_SCHEME_COUNT) ? s_scheme_names[scheme] : "color";
}

int mini_gui_scheme_from_name(const char *name) {
    static const struct { const char *name; MiniGuiScheme scheme; } aliases[] = {
        { "default-16", MINI_GUI_SCHEME_COLOR },  { "16", MINI_GUI_SCHEME_COLOR },
        { "default-256", MINI_GUI_SCHEME_COLOR }, { "256", MINI_GUI_SCHEME_COLOR },
        { "dark", MINI_GUI_SCHEME_GRAY },         { "mono-dark", MINI_GUI_SCHEME_GRAY },
        { "mono", MINI_GUI_SCHEME_GRAY },         { "grey", MINI_GUI_SCHEME_GRAY },
        { "e-ink", MINI_GUI_SCHEME_EINK },        { "bw", MINI_GUI_SCHEME_EINK },
        { "matrix", MINI_GUI_SCHEME_GREEN },      { "phosphor", MINI_GUI_SCHEME_GREEN },
        { "cyberpunk", MINI_GUI_SCHEME_CYAN },    { "blue", MINI_GUI_SCHEME_CYAN },
        { "gameboy-4", MINI_GUI_SCHEME_GAMEBOY }, { "inverse", MINI_GUI_SCHEME_INVERT },
        { "negative", MINI_GUI_SCHEME_INVERT },
    };
    if (!name) return -1;
    for (int i = 0; i < MINI_GUI_SCHEME_COUNT; i++) {
        if (strcmp(name, s_scheme_names[i]) == 0) return i;
    }
    for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++) {
        if (strcmp(name, aliases[i].name) == 0) return (int)aliases[i].scheme;
    }
    return -1;
}

/* ------------------------------------------------------------------------- */
/* Surface storage                                                           */
/* ------------------------------------------------------------------------- */

static size_t gui_row_bytes(int width, int bits) {
    return ((size_t)width * (size_t)bits + 7) / 8;
}

static void surf_free(void) {
#if defined(ESP_PLATFORM)
    for (int i = 0; i < s_surf.nchunks; i++) heap_caps_free(s_surf.chunk[i]);
#endif
    memset(&s_surf, 0, sizeof(s_surf));
}

/* Rows of the current width and bits; one chunk for a band (it goes to DMA
 * whole), several for a full frame. False when memory is short. */
static bool surf_alloc(int rows, bool one_chunk) {
    surf_free();
    if (rows <= 0) return false;
    size_t stride = gui_row_bytes(s_width, s_bits);
#if !defined(ESP_PLATFORM)
    (void)one_chunk;
    if (stride * (size_t)rows > sizeof(s_surf_pool)) return false;
    s_surf.chunk[0] = s_surf_pool;
    s_surf.nchunks = 1;
    s_surf.chunk_rows = rows;
    s_surf.capacity = sizeof(s_surf_pool);
#else
    int per = (int)(GUI_SURF_CHUNK_MAX / stride);
    if (per < 1) per = 1;
    if (one_chunk || per > rows) per = rows;
    if ((rows + per - 1) / per > GUI_SURF_CHUNKS) per = (rows + GUI_SURF_CHUNKS - 1) / GUI_SURF_CHUNKS;
    for (int r = 0; r < rows; r += per) {
        size_t bytes = stride * (size_t)per;
        uint8_t *p = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        if (!p) p = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
        if (!p) {
            surf_free();
            return false;
        }
        s_surf.chunk[s_surf.nchunks++] = p;
        s_surf.capacity += bytes;
    }
    s_surf.chunk_rows = per;
#endif
    s_surf.rows = rows;
    s_surf.stride = stride;
    s_surf.y0 = 0;
    return true;
}

/* Surface row holding screen row y, NULL when not held */
static inline uint8_t *surf_row(int y) {
    int r = y - s_surf.y0;
    if ((unsigned)r >= (unsigned)s_surf.rows) return NULL;
    if (s_surf.nchunks == 1) return s_surf.chunk[0] + (size_t)r * s_surf.stride;
    return s_surf.chunk[r / s_surf.chunk_rows] + (size_t)(r % s_surf.chunk_rows) * s_surf.stride;
}

/* log2 of pixels per byte for 8/4/2/1 bits */
static inline int bits_shift(int bits) {
    return bits == 8 ? 0 : bits == 4 ? 1 : bits == 2 ? 2 : 3;
}

static inline void put_packed(uint8_t *row, int x, int bits, unsigned v) {
    int sh = bits_shift(bits);
    int ppb = 1 << sh;
    int shift = (ppb - 1 - (x & (ppb - 1))) * bits;
    unsigned mask = ((1u << bits) - 1u) << shift;
    uint8_t *p = &row[x >> sh];
    *p = (uint8_t)((*p & ~mask) | ((v << shift) & mask));
}

static inline unsigned get_packed(const uint8_t *row, int x, int bits) {
    int sh = bits_shift(bits);
    int ppb = 1 << sh;
    int shift = (ppb - 1 - (x & (ppb - 1))) * bits;
    return (row[x >> sh] >> shift) & ((1u << bits) - 1u);
}

/* RGB565 run, aligned to 32-bit stores */
static void fill16(uint16_t *p, int count, uint16_t color) {
    if (count <= 0) return;
    if (((uintptr_t)p & 2) != 0) {
        *p++ = color;
        count--;
    }
    uint32_t c32 = ((uint32_t)color << 16) | color;
    uint32_t *w = (uint32_t *)p;
    for (int i = 0; i < count / 2; i++) w[i] = c32;
    if (count & 1) p[count - 1] = color;
}

/* Writes ink over columns [x0, x1) of one surface row */
static void surf_span(uint8_t *row, int x0, int x1, uint16_t ink) {
    if (x0 >= x1) return;
    if (s_bits == 16) {
        fill16((uint16_t *)row + x0, x1 - x0, ink);
        return;
    }
    if (s_bits == 8) {
        memset(row + x0, (int)ink, (size_t)(x1 - x0));
        return;
    }
    int sh = bits_shift(s_bits);
    int ppb = 1 << sh;
    while (x0 < x1 && (x0 & (ppb - 1))) put_packed(row, x0++, s_bits, ink);
    int nbytes = (x1 - x0) >> sh;
    if (nbytes > 0) {
        unsigned b = ink & ((1u << s_bits) - 1u);
        for (int k = s_bits; k < 8; k <<= 1) b |= b << k;
        memset(row + (x0 >> sh), (int)(b & 0xFF), (size_t)nbytes);
        x0 += nbytes << sh;
    }
    while (x0 < x1) put_packed(row, x0++, s_bits, ink);
}

void mini_gui_rows_to_rgb565(const MiniGuiRows *rows, int row, int x, int count, uint16_t *out) {
    if (!rows || !out || count <= 0) return;
    const uint8_t *p = rows->data + (size_t)row * rows->stride;
    const uint16_t *pal = rows->palette;
    int bits = rows->bits;
    if (bits == 16) {
        memcpy(out, (const uint16_t *)(const void *)p + x, (size_t)count * 2);
        return;
    }
    if (bits == 8) {
        for (int i = 0; i < count; i++) out[i] = pal[p[x + i]];
        return;
    }
    /* Packed: unaligned head pixel by pixel, then a whole byte per step */
    int ppb = 8 / bits;
    int i = 0;
    while (i < count && ((x + i) % ppb)) {
        out[i] = pal[get_packed(p, x + i, bits)];
        i++;
    }
    const uint8_t *b = p + (x + i) / ppb;
    if (bits == 4) {
        for (; i + 2 <= count; i += 2, b++) {
            out[i] = pal[*b >> 4];
            out[i + 1] = pal[*b & 15];
        }
    } else if (bits == 2) {
        for (; i + 4 <= count; i += 4, b++) {
            out[i] = pal[*b >> 6];
            out[i + 1] = pal[(*b >> 4) & 3];
            out[i + 2] = pal[(*b >> 2) & 3];
            out[i + 3] = pal[*b & 3];
        }
    } else {
        uint16_t c0 = pal[0], c1 = pal[1];
        for (; i + 8 <= count; i += 8, b++) {
            uint8_t v = *b;
            for (int k = 0; k < 8; k++) out[i + k] = (v & (0x80 >> k)) ? c1 : c0;
        }
    }
    for (; i < count; i++) out[i] = pal[get_packed(p, x + i, bits)];
}

/* ------------------------------------------------------------------------- */
/* Writing pixels                                                            */
/* ------------------------------------------------------------------------- */

static void gui_note_flush(int y0, int y1) {
    if (s_flush_y1 <= s_flush_y0) {
        s_flush_y0 = y0;
        s_flush_y1 = y1;
        return;
    }
    if (y0 < s_flush_y0) s_flush_y0 = y0;
    if (y1 > s_flush_y1) s_flush_y1 = y1;
}

static void gui_direct_fill(int x, int y, int w, int h, uint16_t rgb) {
    if (s_out_sink && s_out_sink->fill) {
        s_out_sink->fill(x, y, w, h, rgb, s_out_ud);
        return;
    }
    if (!s_vfb) return;
    for (int r = 0; r < h; r++) fill16(&s_vfb[(size_t)(y + r) * (size_t)s_width + x], w, rgb);
    mini_gui_mark_dirty_rect(x, y, w, h);
}

/* Fills [x0, x1) x [y0, y1); callers have clipped it to the effective clip */
static void gui_fill_clipped(int x0, int y0, int x1, int y1, uint16_t color) {
    if (x0 >= x1 || y0 >= y1) return;
    if (s_active_sub_buffer) {
        for (int y = y0; y < y1; y++) fill16(&s_target_pixels[(size_t)y * (size_t)s_target_width + x0], x1 - x0, color);
        return;
    }
    if (s_gui_mode == MINI_GUI_MODE_DIRECT) {
        gui_direct_fill(x0, y0, x1 - x0, y1 - y0, gui_panel_color(color));
        return;
    }
    uint16_t ink = gui_ink(color);
    for (int y = y0; y < y1; y++) {
        uint8_t *row = surf_row(y);
        if (row) surf_span(row, x0 - s_surf.x0, x1 - s_surf.x0, ink);
    }
    s_dirty = true;
    if (s_gui_mode == MINI_GUI_MODE_FULL && !s_in_band_render) gui_note_flush(y0, y1);
}

/* One horizontal run [x0, x1) on row y, clipped here */
static void gui_hrun(int x0, int x1, int y, uint16_t color) {
    if (y < s_clip_y0 || y >= s_clip_y1) return;
    if (x0 < s_clip_x0) x0 = s_clip_x0;
    if (x1 > s_clip_x1) x1 = s_clip_x1;
    gui_fill_clipped(x0, y, x1, y + 1, color);
}

/* ------------------------------------------------------------------------- */
/* Panel output                                                              */
/* ------------------------------------------------------------------------- */

void mini_gui_set_output(MiniBandEmitFn emit, const MiniGuiDirectSink *sink, void *userdata) {
    s_out_emit = emit;
    s_out_sink = sink;
    s_out_ud = userdata;
}

void mini_gui_set_rect_output(MiniRectEmitFn emit) {
    s_out_rect = emit;
}

static bool gui_rect_output_ok(void) {
    return s_gui_mode == MINI_GUI_MODE_BAND && s_surf.nchunks == 1 && s_surf.rows > 0 &&
           (s_out_rect || (!s_out_emit && s_vfb));
}

/* Sends the surface (a box redrawn on its own: rows w pixels wide) out */
static void gui_emit_rect(int x, int y, int w, int h) {
    MiniGuiRows rows = { s_surf.chunk[0], s_surf.stride, s_bits, s_palette };
    if (s_out_rect) {
        s_out_rect(&rows, x, y, w, h, s_out_ud);
    } else if (s_vfb) {
        for (int i = 0; i < h; i++) {
            mini_gui_rows_to_rgb565(&rows, i, 0, w, &s_vfb[(size_t)(y + i) * (size_t)s_width + (size_t)x]);
        }
        mini_gui_mark_dirty_rect(x, y, w, h);
    }
}

/* Sends screen rows [y0, y1) of the surface out, one chunk at a time */
static void gui_emit_rows(int y0, int y1, MiniBandEmitFn emit, void *userdata) {
    if (!emit) {
        emit = s_out_emit;
        userdata = s_out_ud;
    }
    if (y0 < s_surf.y0) y0 = s_surf.y0;
    if (y1 > s_surf.y0 + s_surf.rows) y1 = s_surf.y0 + s_surf.rows;
    while (y0 < y1) {
        int r = y0 - s_surf.y0;
        int end = s_surf.y0 + (r / s_surf.chunk_rows + 1) * s_surf.chunk_rows;
        if (end > y1) end = y1;
        MiniGuiRows rows = { surf_row(y0), s_surf.stride, s_bits, s_palette };
        if (emit) {
            emit(&rows, y0, end - y0, s_width, userdata);
        } else if (s_vfb) {
            for (int i = 0; i < end - y0; i++) {
                mini_gui_rows_to_rgb565(&rows, i, 0, s_width, &s_vfb[(size_t)(y0 + i) * (size_t)s_width]);
            }
            mini_gui_mark_dirty_rect(0, y0, s_width, end - y0);
        }
        y0 = end;
    }
}

/* ------------------------------------------------------------------------- */
/* Configuration                                                             */
/* ------------------------------------------------------------------------- */

static void gui_set_window(int y0, int y1);

bool mini_gui_configure(const MiniGuiConfig *cfg) {
    if (!cfg) return false;
    if (cfg->width > 0 && cfg->height > 0 && cfg->width * cfg->height <= GUI_MAX_PIXELS) {
        s_width = cfg->width;
        s_height = cfg->height;
    }
    int bits = cfg->color_bits;
    s_bits = (bits == 1 || bits == 2 || bits == 4 || bits == 8) ? bits : 16;
    s_scheme = (cfg->scheme >= 0 && cfg->scheme < MINI_GUI_SCHEME_COUNT) ? cfg->scheme : MINI_GUI_SCHEME_COLOR;
    s_gui_color_format = cfg->bus == MINI_GUI_COLOR_RGB666 ? MINI_GUI_COLOR_RGB666 : MINI_GUI_COLOR_RGB565;
    s_gui_band_lines = (cfg->band_lines > 0 && cfg->band_lines <= s_height) ? cfg->band_lines : 20;
    gui_build_palette();

    MiniGuiMode mode = cfg->mode;
    bool ok = true;
    if (mode == MINI_GUI_MODE_FULL) {
        ok = surf_alloc(s_height, false);
        if (!ok) mode = MINI_GUI_MODE_BAND; /* not enough RAM for the frame */
    }
    if (mode == MINI_GUI_MODE_BAND && !surf_alloc(s_gui_band_lines, true)) {
        ok = false;
        mode = MINI_GUI_MODE_DIRECT;
    }
    if (mode == MINI_GUI_MODE_DIRECT) surf_free();
    s_gui_mode = mode;
    s_flush_y0 = s_flush_y1 = 0;

    s_active_sub_buffer = NULL;
    s_target_pixels = NULL;
    s_target_width = s_width;
    s_target_height = s_height;
    if (mode == MINI_GUI_MODE_BAND) gui_set_window(0, s_surf.rows);
    else gui_set_window(0, s_height);
    mini_gui_clear(MINI_COLOR_BG_DARK);
    if (mode == MINI_GUI_MODE_FULL) gui_emit_rows(0, s_height, NULL, NULL);
    mini_gui_invalidate(0, s_height);
    return ok;
}

void mini_gui_get_config(MiniGuiConfig *out) {
    if (!out) return;
    out->width = s_width;
    out->height = s_height;
    out->mode = s_gui_mode;
    out->band_lines = s_gui_band_lines;
    out->color_bits = s_bits;
    out->scheme = s_scheme;
    out->bus = s_gui_color_format;
}

/* Changes one setting, keeping the others */
static void gui_reconfigure(MiniGuiMode mode, int bits, MiniGuiScheme scheme) {
    MiniGuiConfig cfg;
    mini_gui_get_config(&cfg);
    cfg.mode = mode;
    cfg.color_bits = bits;
    cfg.scheme = scheme;
    mini_gui_configure(&cfg);
}

void mini_gui_init_config(int width, int height, MiniGuiMode mode, int band_lines, MiniGuiColorFormat color_fmt) {
    MiniGuiConfig cfg;
    mini_gui_get_config(&cfg);
    cfg.width = width;
    cfg.height = height;
    cfg.mode = mode;
    cfg.band_lines = band_lines;
    if (color_fmt == MINI_GUI_COLOR_4BPP) cfg.color_bits = 4; /* legacy */
    else cfg.bus = color_fmt;
    mini_gui_configure(&cfg);
}

void mini_gui_init(int width, int height) {
    mini_gui_init_config(width, height, MINI_GUI_MODE_DIRECT, 20, MINI_GUI_COLOR_RGB565);
}

void mini_gui_set_fb_enabled(bool enabled) {
    if ((s_gui_mode == MINI_GUI_MODE_FULL) == enabled) return;
    gui_reconfigure(enabled ? MINI_GUI_MODE_FULL : MINI_GUI_MODE_DIRECT, s_bits, s_scheme);
}

bool mini_gui_is_fb_enabled(void) {
    return s_gui_mode == MINI_GUI_MODE_FULL;
}

void mini_gui_set_color_bits(int bits) {
    if (bits != 1 && bits != 2 && bits != 4 && bits != 8 && bits != 16) return;
    if (bits == s_bits) return;
    gui_reconfigure(s_gui_mode, bits, s_scheme);
}

int mini_gui_get_color_bits(void) {
    return s_bits;
}

void mini_gui_set_color_depth(int bpp) {
    mini_gui_set_color_bits(bpp);
}

int mini_gui_get_color_depth(void) {
    return s_bits;
}

void mini_gui_set_scheme(MiniGuiScheme scheme) {
    if (scheme < 0 || scheme >= MINI_GUI_SCHEME_COUNT) return;
    s_scheme = scheme;
    gui_build_palette();
    mini_gui_invalidate(0, s_height);
}

MiniGuiScheme mini_gui_get_scheme(void) {
    return s_scheme;
}

size_t mini_gui_get_fb_ram(void) {
    return s_gui_mode == MINI_GUI_MODE_DIRECT ? 0 : s_surf.stride * (size_t)s_surf.rows;
}

/* Legacy two-color API: a custom palette with bg first */
void mini_gui_set_mono_colors(uint16_t fg, uint16_t bg) {
    uint16_t *cp = custom_pal();
    if (!cp) return;
    cp[0] = bg;
    cp[1] = fg;
    mini_gui_set_scheme(MINI_GUI_SCHEME_CUSTOM);
}

void mini_gui_get_mono_colors(uint16_t *out_fg, uint16_t *out_bg) {
    int n = s_pal_n > 0 ? s_pal_n : 1;
    if (out_fg) *out_fg = s_pal_n > 0 ? s_palette[n - 1] : MINI_COLOR_WHITE;
    if (out_bg) *out_bg = s_pal_n > 0 ? s_palette[0] : MINI_COLOR_BG_DARK;
    const uint16_t *cp = s_scheme == MINI_GUI_SCHEME_CUSTOM ? custom_pal() : NULL;
    if (cp) {
        if (out_fg) *out_fg = cp[1];
        if (out_bg) *out_bg = cp[0];
    }
}

/* Editing an entry switches to the custom scheme, starting from the current palette */
void mini_gui_set_palette(int index, uint16_t rgb565) {
    if (index < 0 || index >= 256) return;
    uint16_t *cp = custom_pal();
    if (!cp) return;
    if (s_scheme != MINI_GUI_SCHEME_CUSTOM) {
        for (int i = 0; i < s_pal_n; i++) cp[i] = s_palette[i];
    }
    cp[index] = rgb565;
    mini_gui_set_scheme(MINI_GUI_SCHEME_CUSTOM);
}

uint16_t mini_gui_get_palette(int index) {
    if (index < 0 || index >= 256) return 0;
    if (index < s_pal_n) return s_palette[index];
    const uint16_t *cp = s_scheme == MINI_GUI_SCHEME_CUSTOM ? custom_pal() : NULL;
    return cp ? cp[index] : 0;
}

void mini_gui_set_palette_preset(int preset_id) {
    static const MiniGuiScheme presets[8] = {
        MINI_GUI_SCHEME_COLOR, MINI_GUI_SCHEME_EINK, MINI_GUI_SCHEME_GRAY, MINI_GUI_SCHEME_GREEN,
        MINI_GUI_SCHEME_AMBER, MINI_GUI_SCHEME_CYAN, MINI_GUI_SCHEME_GAMEBOY, MINI_GUI_SCHEME_COLOR,
    };
    if (preset_id >= 0 && preset_id < 8) mini_gui_set_scheme(presets[preset_id]);
}

void mini_gui_requantize(void) {
    mini_gui_invalidate(0, s_height);
}

MiniGuiMode mini_gui_get_mode(void) { return s_gui_mode; }

int mini_gui_get_band_lines(void) {
    return s_gui_mode == MINI_GUI_MODE_BAND && s_surf.rows > 0 ? s_surf.rows : s_gui_band_lines;
}

MiniGuiColorFormat mini_gui_get_color_format(void) { return s_gui_color_format; }

uint16_t *mini_gui_get_band_buffer(void) {
    return (s_gui_mode == MINI_GUI_MODE_BAND && s_bits == 16) ? (uint16_t *)(void *)s_surf.chunk[0] : NULL;
}

void mini_gui_set_resolution(int width, int height) {
    if (width <= 0 || height <= 0 || width * height > GUI_MAX_PIXELS) return;
    if (width == s_width && height == s_height) return;
    MiniGuiConfig cfg;
    mini_gui_get_config(&cfg);
    cfg.width = width;
    cfg.height = height;
    if (s_gui_mode == MINI_GUI_MODE_BAND) {
        /* A wider rotation gets fewer lines: the band keeps its RAM */
        size_t stride = gui_row_bytes(width, s_bits);
        int fit = stride ? (int)(s_surf.capacity / stride) : 0;
        if (fit > 0 && fit < cfg.band_lines) cfg.band_lines = fit;
    }
    mini_gui_configure(&cfg);
}

/* Peripheral setters keep the simulator mirror and, on ESP32, drive the
 * real hardware (previously only the mirror was updated) */
void mini_ws2812_set(uint8_t r, uint8_t g, uint8_t b) {
    s_ws2812_r = r;
    s_ws2812_g = g;
    s_ws2812_b = b;
#if defined(ESP_PLATFORM)
    esp32_ws2812_set(r, g, b);
#endif
}

void mini_ws2812_get(uint8_t *r, uint8_t *g, uint8_t *b) {
    if (r) *r = s_ws2812_r;
    if (g) *g = s_ws2812_g;
    if (b) *b = s_ws2812_b;
}

void mini_light_set(int val) {
    if (val < 0) val = 0;
    if (val > 4095) val = 4095;
    s_light_val = val;
}

int mini_light_read(void) {
    return s_light_val;
}

void mini_backlight_set(uint8_t val) {
    s_backlight_val = val;
#if defined(ESP_PLATFORM)
    esp32_backlight_set(val);
#endif
}

uint8_t mini_backlight_get(void) {
    return s_backlight_val;
}

/* Automatic backlight from the light sensor (IO35), stepped once a second
 * (mini_device_clock_tick); (backlight-set n) turns it off. A host hook
 * keeps the choice (config.ini [display] auto_backlight / backlight). */
static bool s_backlight_auto = true;
static int  s_backlight_offset = 0;   /* -10 to 10 */
static int  s_backlight_min = 30;     /* 0 to 255 */
static int  s_backlight_max = 255;    /* 0 to 255 */
static void (*s_backlight_hook)(bool auto_on, int level) = NULL;

bool mini_backlight_auto(void) {
    return s_backlight_auto;
}

void mini_backlight_auto_set(bool on) {
    s_backlight_auto = on;
}

void mini_backlight_set_params(int offset, int min_val, int max_val) {
    if (offset < -10) offset = -10;
    if (offset > 10) offset = 10;
    if (min_val < 0) min_val = 0;
    if (min_val > 255) min_val = 255;
    if (max_val < 0) max_val = 0;
    if (max_val > 255) max_val = 255;
    if (min_val > max_val) {
        int tmp = min_val;
        min_val = max_val;
        max_val = tmp;
    }
    s_backlight_offset = offset;
    s_backlight_min = min_val;
    s_backlight_max = max_val;
}

int mini_backlight_get_offset(void) {
    return s_backlight_offset;
}

int mini_backlight_get_min(void) {
    return s_backlight_min;
}

int mini_backlight_get_max(void) {
    return s_backlight_max;
}

int mini_backlight_calc_dynamic(int light_raw, int min_val, int max_val, int offset) {
    if (min_val < 0) min_val = 0;
    if (min_val > 255) min_val = 255;
    if (max_val < 0) max_val = 0;
    if (max_val > 255) max_val = 255;
    if (min_val > max_val) {
        int tmp = min_val;
        min_val = max_val;
        max_val = tmp;
    }
    if (offset < -10) offset = -10;
    if (offset > 10) offset = 10;
    if (light_raw <= 0) return min_val;
    if (light_raw >= 4095) return max_val;

    double frac = (double)light_raw / 4095.0;
    /* Gamma centered around 0.5 (square-root human eye response at offset 0).
     * Offset > 0 decreases gamma (brighter curve), offset < 0 increases gamma (dimmer curve).
     * 2^(-offset / 5.0) == exp(-offset * ln(2) / 5.0). */
    double gamma = 0.5 * exp(-(double)offset * (0.6931471805599453 / 5.0));
    double ratio = pow(frac, gamma);
    int target = min_val + (int)((double)(max_val - min_val) * ratio + 0.5);
    if (target < min_val) target = min_val;
    if (target > max_val) target = max_val;
    if (target < 0) target = 0;
    if (target > 255) target = 255;
    return target;
}

void mini_backlight_set_hook(void (*hook)(bool auto_on, int level)) {
    s_backlight_hook = hook;
}

/* Dynamic brightness curve bounded by [min, max] with offset exponent; each
 * step goes half way, so the light changes smoothly */
void mini_backlight_auto_step(void) {
    if (!s_backlight_auto) return;
    int target = mini_backlight_calc_dynamic(mini_light_read(), s_backlight_min, s_backlight_max, s_backlight_offset);
    int now = s_backlight_val;
    int next = now + (target - now) / 2;
    if (next == now && now != target) next = target;
    if (next != now) mini_backlight_set((uint8_t)next);
}

void mini_button_set(bool pressed) {
    s_button_pressed = pressed;
}

bool mini_button_read(void) {
    return s_button_pressed;
}

/* ========================================================================= */
/* Clipping Region & Dirty Rectangle Management                             */
/* ========================================================================= */

/* Logical drawing surface size: sub-buffer when targeted, else the full screen.
 * Band mode does not shrink it; bands only restrict the effective clip. */
static void gui_logical_size(int *w, int *h) {
    if (s_active_sub_buffer) {
        *w = s_active_sub_buffer->width;
        *h = s_active_sub_buffer->height;
    } else {
        *w = s_width;
        *h = s_height;
    }
}

/* Recompute effective clip: user clip intersected with the render window. */
static void gui_apply_clip(void) {
    s_clip_x0 = s_uclip_x0;
    s_clip_y0 = s_uclip_y0;
    s_clip_x1 = s_uclip_x1;
    s_clip_y1 = s_uclip_y1;
    if (!s_active_sub_buffer) {
        if (s_clip_y0 < s_win_y0) s_clip_y0 = s_win_y0;
        if (s_clip_y1 > s_win_y1) s_clip_y1 = s_win_y1;
        if (s_clip_x0 < s_win_x0) s_clip_x0 = s_win_x0;
        if (s_clip_x1 > s_win_x1) s_clip_x1 = s_win_x1;
        if (s_clip_x1 > s_width) s_clip_x1 = s_width;
    }
    if (s_clip_x0 > s_clip_x1) s_clip_x0 = s_clip_x1;
    if (s_clip_y0 > s_clip_y1) s_clip_y0 = s_clip_y1;
}

void mini_gui_set_clip(int x, int y, int w, int h) {
    if (s_rec_on) gui_rec(GUI_OP_SET_CLIP, x, y, w, h, 0, 0, 0, 0, 0, 0, NULL);
    if (w <= 0 || h <= 0) {
        s_uclip_x0 = s_uclip_x1 = 0;
        s_uclip_y0 = s_uclip_y1 = 0;
        gui_apply_clip();
        return;
    }
    int max_w, max_h;
    gui_logical_size(&max_w, &max_h);
    s_uclip_x0 = x < 0 ? 0 : x;
    s_uclip_y0 = y < 0 ? 0 : y;
    s_uclip_x1 = (x + w) > max_w ? max_w : (x + w);
    s_uclip_y1 = (y + h) > max_h ? max_h : (y + h);
    if (s_uclip_x0 > s_uclip_x1) s_uclip_x0 = s_uclip_x1;
    if (s_uclip_y0 > s_uclip_y1) s_uclip_y0 = s_uclip_y1;
    gui_apply_clip();
}

void mini_gui_reset_clip(void) {
    if (s_rec_on) gui_rec(GUI_OP_RESET_CLIP, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, NULL);
    int max_w, max_h;
    gui_logical_size(&max_w, &max_h);
    s_uclip_x0 = 0;
    s_uclip_y0 = 0;
    s_uclip_x1 = max_w;
    s_uclip_y1 = max_h;
    gui_apply_clip();
}

void mini_gui_get_clip(int *out_x, int *out_y, int *out_w, int *out_h) {
    if (out_x) *out_x = s_uclip_x0;
    if (out_y) *out_y = s_uclip_y0;
    if (out_w) *out_w = (s_uclip_x1 > s_uclip_x0) ? (s_uclip_x1 - s_uclip_x0) : 0;
    if (out_h) *out_h = (s_uclip_y1 > s_uclip_y0) ? (s_uclip_y1 - s_uclip_y0) : 0;
}

void mini_gui_mark_dirty_rect(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = (x + w) > s_width ? s_width : (x + w);
    int y1 = (y + h) > s_height ? s_height : (y + h);
    if (x0 >= x1 || y0 >= y1) return;

    if (!s_dirty) {
        s_dirty_x0 = x0;
        s_dirty_y0 = y0;
        s_dirty_x1 = x1;
        s_dirty_y1 = y1;
        s_dirty = true;
    } else {
        if (x0 < s_dirty_x0) s_dirty_x0 = x0;
        if (y0 < s_dirty_y0) s_dirty_y0 = y0;
        if (x1 > s_dirty_x1) s_dirty_x1 = x1;
        if (y1 > s_dirty_y1) s_dirty_y1 = y1;
    }

    /* Mark tiles in 32x32 tile grid */
    int tiles_x = (s_width + MINI_GUI_TILE_SIZE - 1) / MINI_GUI_TILE_SIZE;
    int tx0 = x0 / MINI_GUI_TILE_SIZE;
    int ty0 = y0 / MINI_GUI_TILE_SIZE;
    int tx1 = (x1 - 1) / MINI_GUI_TILE_SIZE;
    int ty1 = (y1 - 1) / MINI_GUI_TILE_SIZE;
    if (tx1 >= tiles_x) tx1 = tiles_x - 1;
    int tiles_y = (s_height + MINI_GUI_TILE_SIZE - 1) / MINI_GUI_TILE_SIZE;
    if (ty1 >= tiles_y) ty1 = tiles_y - 1;

    for (int ty = ty0; ty <= ty1; ty++) {
        for (int tx = tx0; tx <= tx1; tx++) {
            int t_idx = ty * tiles_x + tx;
            if (t_idx >= 0 && t_idx < MINI_GUI_TOTAL_TILES) {
                s_dirty_tiles[t_idx / 32] |= (1U << (t_idx % 32));
            }
        }
    }
    s_coalesced_dirty_valid = false;
}

bool mini_gui_get_dirty_rect(int *out_x, int *out_y, int *out_w, int *out_h) {
    if (!s_dirty) {
        if (out_x) *out_x = 0;
        if (out_y) *out_y = 0;
        if (out_w) *out_w = 0;
        if (out_h) *out_h = 0;
        return false;
    }
    if (out_x) *out_x = s_dirty_x0;
    if (out_y) *out_y = s_dirty_y0;
    if (out_w) *out_w = (s_dirty_x1 > s_dirty_x0) ? (s_dirty_x1 - s_dirty_x0) : 0;
    if (out_h) *out_h = (s_dirty_y1 > s_dirty_y0) ? (s_dirty_y1 - s_dirty_y0) : 0;
    return true;
}

static void recompute_coalesced_dirty_rects(void) {
    if (s_coalesced_dirty_valid) return;
    s_coalesced_dirty_count = 0;
    if (!s_dirty) {
        s_coalesced_dirty_valid = true;
        return;
    }

    int tiles_x = (s_width + MINI_GUI_TILE_SIZE - 1) / MINI_GUI_TILE_SIZE;
    int tiles_y = (s_height + MINI_GUI_TILE_SIZE - 1) / MINI_GUI_TILE_SIZE;
    if (tiles_x > MINI_GUI_MAX_TILES_X) tiles_x = MINI_GUI_MAX_TILES_X;
    if (tiles_y > MINI_GUI_MAX_TILES_Y) tiles_y = MINI_GUI_MAX_TILES_Y;

    typedef struct TileRun {
        int x0, x1, y;
    } TileRun;
    TileRun runs[64];
    int run_count = 0;

    for (int ty = 0; ty < tiles_y && run_count < 64; ty++) {
        int start_tx = -1;
        for (int tx = 0; tx <= tiles_x; tx++) {
            bool is_dirty_tile = false;
            if (tx < tiles_x) {
                int t_idx = ty * tiles_x + tx;
                if (t_idx < MINI_GUI_TOTAL_TILES && (s_dirty_tiles[t_idx / 32] & (1U << (t_idx % 32)))) {
                    is_dirty_tile = true;
                }
            }
            if (is_dirty_tile) {
                if (start_tx < 0) start_tx = tx;
            } else {
                if (start_tx >= 0) {
                    if (run_count < 64) {
                        runs[run_count].x0 = start_tx;
                        runs[run_count].x1 = tx - 1;
                        runs[run_count].y = ty;
                        run_count++;
                    }
                    start_tx = -1;
                }
            }
        }
    }

    typedef struct RectRun {
        int tx0, tx1, ty0, ty1;
        bool merged;
    } RectRun;
    RectRun rects[64];
    int rect_count = run_count;

    for (int i = 0; i < run_count; i++) {
        rects[i].tx0 = runs[i].x0;
        rects[i].tx1 = runs[i].x1;
        rects[i].ty0 = runs[i].y;
        rects[i].ty1 = runs[i].y;
        rects[i].merged = false;
    }

    for (int i = 0; i < rect_count; i++) {
        if (rects[i].merged) continue;
        for (int j = i + 1; j < rect_count; j++) {
            if (rects[j].merged) continue;
            if (rects[i].tx0 == rects[j].tx0 && rects[i].tx1 == rects[j].tx1 && rects[i].ty1 + 1 == rects[j].ty0) {
                rects[i].ty1 = rects[j].ty1;
                rects[j].merged = true;
            }
        }
    }

    int active_count = 0;
    for (int i = 0; i < rect_count && active_count < MINI_GUI_MAX_DIRTY_RECTS; i++) {
        if (rects[i].merged) continue;
        int px0 = rects[i].tx0 * MINI_GUI_TILE_SIZE;
        int py0 = rects[i].ty0 * MINI_GUI_TILE_SIZE;
        int px1 = (rects[i].tx1 + 1) * MINI_GUI_TILE_SIZE;
        int py1 = (rects[i].ty1 + 1) * MINI_GUI_TILE_SIZE;

        if (px0 < s_dirty_x0) px0 = s_dirty_x0;
        if (py0 < s_dirty_y0) py0 = s_dirty_y0;
        if (px1 > s_dirty_x1) px1 = s_dirty_x1;
        if (py1 > s_dirty_y1) py1 = s_dirty_y1;

        if (px1 > px0 && py1 > py0) {
            s_coalesced_dirty_rects[active_count].x = px0;
            s_coalesced_dirty_rects[active_count].y = py0;
            s_coalesced_dirty_rects[active_count].w = px1 - px0;
            s_coalesced_dirty_rects[active_count].h = py1 - py0;
            active_count++;
        }
    }

    if (active_count == 0 && s_dirty) {
        s_coalesced_dirty_rects[0].x = s_dirty_x0;
        s_coalesced_dirty_rects[0].y = s_dirty_y0;
        s_coalesced_dirty_rects[0].w = (s_dirty_x1 > s_dirty_x0) ? (s_dirty_x1 - s_dirty_x0) : 0;
        s_coalesced_dirty_rects[0].h = (s_dirty_y1 > s_dirty_y0) ? (s_dirty_y1 - s_dirty_y0) : 0;
        active_count = (s_coalesced_dirty_rects[0].w > 0 && s_coalesced_dirty_rects[0].h > 0) ? 1 : 0;
    }

    s_coalesced_dirty_count = active_count;
    s_coalesced_dirty_valid = true;
}

int mini_gui_get_dirty_rect_count(void) {
    if (!s_dirty) return 0;
    recompute_coalesced_dirty_rects();
    return s_coalesced_dirty_count;
}

bool mini_gui_get_dirty_rect_at(int idx, int *out_x, int *out_y, int *out_w, int *out_h) {
    if (!s_dirty) return false;
    recompute_coalesced_dirty_rects();
    if (idx < 0 || idx >= s_coalesced_dirty_count) return false;
    if (out_x) *out_x = s_coalesced_dirty_rects[idx].x;
    if (out_y) *out_y = s_coalesced_dirty_rects[idx].y;
    if (out_w) *out_w = s_coalesced_dirty_rects[idx].w;
    if (out_h) *out_h = s_coalesced_dirty_rects[idx].h;
    return true;
}

void mini_gui_clear_dirty(void) {
    s_dirty = false;
    s_dirty_x0 = s_width;
    s_dirty_y0 = s_height;
    s_dirty_x1 = 0;
    s_dirty_y1 = 0;
    memset(s_dirty_tiles, 0, sizeof(s_dirty_tiles));
    s_coalesced_dirty_valid = true;
    s_coalesced_dirty_count = 0;
}

/* ========================================================================= */
/* Scanline / Banding Slicing Render Mode                                    */
/* ========================================================================= */

/* Restricts drawing to rows [y0, y1) and resets the user clip */
static void gui_set_window(int y0, int y1) {
    s_win_x0 = 0;
    s_win_x1 = s_width;
    s_win_y0 = y0 < 0 ? 0 : y0;
    s_win_y1 = y1 > s_height ? s_height : y1;
    if (!s_active_sub_buffer) mini_gui_reset_clip();
}

/* A box redrawn on its own: drawing is limited to its columns too */
static void gui_set_window_rect(int x0, int y0, int x1, int y1) {
    gui_set_window(y0, y1);
    s_win_x0 = x0;
    s_win_x1 = x1;
    if (!s_active_sub_buffer) mini_gui_reset_clip();
}

/* Legacy: band_pixels and band_stride are ignored, the band surface is used */
void mini_gui_set_band_mode(bool enabled, int band_y, int band_h, uint16_t *band_pixels, int band_stride) {
    (void)band_pixels;
    (void)band_stride;
    if (enabled) {
        if (s_gui_mode == MINI_GUI_MODE_BAND) s_surf.y0 = band_y;
        gui_set_window(band_y, band_y + band_h);
    } else {
        if (s_gui_mode == MINI_GUI_MODE_BAND) s_surf.y0 = 0;
        gui_set_window(0, s_gui_mode == MINI_GUI_MODE_BAND ? s_surf.rows : s_height);
    }
}

bool mini_gui_is_band_mode(void) {
    return s_gui_mode == MINI_GUI_MODE_BAND;
}

/* Draws the screen slice by slice: BAND sends each slice out, FULL flushes
 * the frame at the end, DIRECT has already drawn to the panel */
static void gui_slices_rows(int y0, int y1, int band_h, void (*draw)(int idx, int y, int h, void *ud), void *ud) {
    if (band_h <= 0 || !draw) return;
    if (s_gui_mode == MINI_GUI_MODE_BAND && band_h > s_surf.rows) band_h = s_surf.rows;
    if (band_h > s_height) band_h = s_height;
    if (y0 < 0) y0 = 0;
    if (y1 > s_height) y1 = s_height;
    if (band_h <= 0 || y0 >= y1) return;
    bool was = s_in_band_render;
    s_in_band_render = true;
    for (int b = 0, y = y0; y < y1; b++, y += band_h) {
        int h = (y + band_h > y1) ? (y1 - y) : band_h;
        if (s_gui_mode == MINI_GUI_MODE_BAND) {
            s_surf.y0 = y;
            gui_set_window(y, y + h);
            mini_gui_clear(MINI_COLOR_BLACK);
        } else {
            gui_set_window(y, y + h);
        }
        draw(b, y, h, ud);
        if (s_gui_mode == MINI_GUI_MODE_BAND) gui_emit_rows(y, y + h, NULL, NULL);
    }
    s_in_band_render = was;
    mini_gui_set_band_mode(false, 0, 0, NULL, 0);
    if (s_gui_mode == MINI_GUI_MODE_FULL) gui_emit_rows(y0, y1, NULL, NULL);
}

static void gui_slices(int band_h, void (*draw)(int idx, int y, int h, void *ud), void *ud) {
    gui_slices_rows(0, s_height, band_h, draw, ud);
}

typedef struct {
    MiniBandDrawFn fn;
    void *userdata;
} GuiSliceFn;

static void gui_slice_c(int idx, int y, int h, void *ud) {
    GuiSliceFn *f = (GuiSliceFn *)ud;
    f->fn(idx, y, h, f->userdata);
}

void mini_gui_render_bands(int band_h, MiniBandDrawFn fn, void *userdata) {
    if (s_rec_on) { s_rec_overflow = true; return; }
    if (!fn) return;
    GuiSliceFn f = { fn, userdata };
    gui_slices(band_h, gui_slice_c, &f);
}

void mini_gui_render_band_rows(int y0, int y1, int band_h, MiniBandDrawFn fn, void *userdata) {
    if (s_rec_on) { s_rec_overflow = true; return; }
    if (!fn) return;
    GuiSliceFn f = { fn, userdata };
    gui_slices_rows(y0, y1, band_h, gui_slice_c, &f);
}

/* ========================================================================= */
/* Retained-Mode Band Pipeline (Invalidation + Shared Band Renderer)         */
/* ========================================================================= */

/* Pending redraw spans [y0, y1). Spans that share a band are merged so a
 * band is never rendered twice; distant spans stay separate so an update at
 * the top and one at the bottom do not repaint everything in between. */
#define GUI_MAX_INVAL_SPANS 4
static int  s_inval_y0[GUI_MAX_INVAL_SPANS];
static int  s_inval_y1[GUI_MAX_INVAL_SPANS];
static int  s_inval_count = 0;
static char s_band_error[160] = {0};

/* True when the band-aligned extents of two spans touch or overlap */
static bool inval_spans_touch(int a0, int a1, int b0, int b1) {
    int band = s_gui_band_lines > 0 ? s_gui_band_lines : 1;
    int a_lo = (a0 / band) * band, a_hi = ((a1 + band - 1) / band) * band;
    int b_lo = (b0 / band) * band, b_hi = ((b1 + band - 1) / band) * band;
    return a_lo <= b_hi && b_lo <= a_hi;
}

static void gui_queue_rows(int y, int h);

void mini_gui_forget_recording(void) {
    s_dl_fresh = false;
}

void mini_gui_invalidate(int y, int h) {
    if (s_in_band_render) return;
    s_dl_fresh = false; /* the app changed something: run the render procedure */
    gui_queue_rows(y, h);
}

static void gui_queue_rows(int y, int h) {
    if (s_in_band_render || h <= 0) return;
    int y0 = y < 0 ? 0 : y;
    int y1 = (y + h) > s_height ? s_height : (y + h);
    if (y0 >= y1) return;
    /* Absorb every span the new one touches */
    for (int i = 0; i < s_inval_count;) {
        if (inval_spans_touch(y0, y1, s_inval_y0[i], s_inval_y1[i])) {
            if (s_inval_y0[i] < y0) y0 = s_inval_y0[i];
            if (s_inval_y1[i] > y1) y1 = s_inval_y1[i];
            s_inval_count--;
            s_inval_y0[i] = s_inval_y0[s_inval_count];
            s_inval_y1[i] = s_inval_y1[s_inval_count];
        } else {
            i++;
        }
    }
    if (s_inval_count == GUI_MAX_INVAL_SPANS) {
        /* Table full: fold into the nearest span */
        int best = 0, best_d = 1 << 30;
        for (int i = 0; i < s_inval_count; i++) {
            int d = (y0 > s_inval_y1[i]) ? y0 - s_inval_y1[i] : (s_inval_y0[i] > y1 ? s_inval_y0[i] - y1 : 0);
            if (d < best_d) { best_d = d; best = i; }
        }
        if (s_inval_y0[best] < y0) y0 = s_inval_y0[best];
        if (s_inval_y1[best] > y1) y1 = s_inval_y1[best];
        s_inval_y0[best] = y0;
        s_inval_y1[best] = y1;
        return;
    }
    s_inval_y0[s_inval_count] = y0;
    s_inval_y1[s_inval_count] = y1;
    s_inval_count++;
}

/* Pops the topmost pending span; call repeatedly until it returns false */
bool mini_gui_take_invalidated(int *out_y0, int *out_y1) {
    if (s_inval_count == 0) return false;
    int top = 0;
    for (int i = 1; i < s_inval_count; i++) {
        if (s_inval_y0[i] < s_inval_y0[top]) top = i;
    }
    if (out_y0) *out_y0 = s_inval_y0[top];
    if (out_y1) *out_y1 = s_inval_y1[top];
    s_inval_count--;
    s_inval_y0[top] = s_inval_y0[s_inval_count];
    s_inval_y1[top] = s_inval_y1[s_inval_count];
    return true;
}

const char *mini_gui_band_last_error(void) {
    return s_band_error;
}

static Value gui_lookup_render_proc(MiniVM *vm) {
    /* env_lookup_name returns void for unbound names without raising */
    Value fn = env_lookup_name(vm, "gui-render-proc");
    if (mini_is_closure(fn) || mini_is_cfunc(fn)) return fn;
    return mini_void();
}

/* Button hold feedback (mini_gui_set_hold_overlay): up to three short bars
 * in the bottom-right corner drawn over whatever the app shows. Bottom: the
 * long press; above it: the mid press; on top: the hold (the hardware
 * pages). Pale green while filling, red once armed (the action runs on
 * release). */
#define HOLD_W        64
#define HOLD_H        4
#define HOLD_MARGIN   5
#define HOLD_GAP      4
#define HOLD_TRACK    10697   /* #2a3a4d */
#define HOLD_FILL     40790   /* #9be8b5, pale green */
#define HOLD_ARMED    60107   /* #ee5a5a, red */
static struct {
    uint8_t flags;            /* MINI_HOLD_* */
    int16_t long_px, mid_px, top_px;   /* filled widths */
} s_hold;

static int hold_bar_y(int bar) {  /* 0: the long press, 1: the mid press, 2: the hold */
    return s_height - HOLD_MARGIN - HOLD_H - bar * (HOLD_H + HOLD_GAP);
}

static void gui_hold_bar(int bar, int px, bool armed) {
    int x = s_width - HOLD_MARGIN - HOLD_W, y = hold_bar_y(bar);
    mini_gui_rect(x - 1, y - 1, HOLD_W + 2, HOLD_H + 2, HOLD_TRACK, true);
    if (armed) mini_gui_rect(x, y, HOLD_W, HOLD_H, HOLD_ARMED, true);
    else if (px > 0) mini_gui_rect(x, y, px, HOLD_H, HOLD_FILL, true);
}

/* Over the band just drawn, before it goes out */
static void gui_draw_hold_overlay(void) {
    if (!(s_hold.flags & MINI_HOLD_VISIBLE)) return;
    gui_hold_bar(0, s_hold.long_px, (s_hold.flags & MINI_HOLD_LONG_ARMED) != 0);
    if (s_hold.flags & MINI_HOLD_MID_VISIBLE)
        gui_hold_bar(1, s_hold.mid_px, (s_hold.flags & MINI_HOLD_MID_ARMED) != 0);
    if (s_hold.flags & MINI_HOLD_INFO_VISIBLE)
        gui_hold_bar(2, s_hold.top_px, (s_hold.flags & MINI_HOLD_INFO_ARMED) != 0);
}

static void gui_queue_rows(int y, int h);

static int hold_px(int pm) { return pm <= 0 ? 0 : (pm >= 1000 ? HOLD_W : pm * HOLD_W / 1000); }

void mini_gui_set_hold_overlay(int flags, int long_pm, int mid_pm, int hold_pm) {
    int lp = hold_px(long_pm), mp = hold_px(mid_pm), tp = hold_px(hold_pm);
    if ((uint8_t)flags == s_hold.flags && lp == s_hold.long_px && mp == s_hold.mid_px && tp == s_hold.top_px) return;
    s_hold.flags = (uint8_t)flags;
    s_hold.long_px = (int16_t)lp;
    s_hold.mid_px = (int16_t)mp;
    s_hold.top_px = (int16_t)tp;
    /* redraw the corner rows only; the recorded display list stays valid */
    int top = hold_bar_y(2) - 1;
    gui_queue_rows(top, s_height - top);
}

int mini_gui_hold_overlay(int *long_px, int *mid_px, int *top_px) {
    if (long_px) *long_px = s_hold.long_px;
    if (mid_px) *mid_px = s_hold.mid_px;
    if (top_px) *top_px = s_hold.top_px;
    return s_hold.flags;
}

/* Re-executes the recorded drawing calls against the current band */
static void gui_dl_replay(void) {
    for (int i = 0; i < s_dl_count; i++) {
        GuiCmd cmd;
        dl_load(&s_dl_cmds[i], &cmd);
        const GuiCmd *c = &cmd;
        const char *str = (c->str != 0xFFFF) ? &s_dl_strings[c->str] : NULL;
        switch (c->op) {
            case GUI_OP_CLEAR:       mini_gui_clear(c->c1); break;
            case GUI_OP_PIXEL:       mini_gui_pixel(c->x, c->y, c->c1); break;
            case GUI_OP_LINE:        mini_gui_line(c->x, c->y, c->w, c->h, c->c1); break;
            case GUI_OP_RECT:        mini_gui_rect(c->x, c->y, c->w, c->h, c->c1, c->b != 0); break;
            case GUI_OP_RRECT:       mini_gui_round_rect(c->x, c->y, c->w, c->h, c->p, c->c1, c->b != 0); break;
            case GUI_OP_CARD:        mini_gui_card(c->x, c->y, c->w, c->h, c->p, c->c1, c->c2); break;
            case GUI_OP_CIRCLE:      mini_gui_circle(c->x, c->y, c->p, c->c1, c->b != 0); break;
            case GUI_OP_TEXT:        mini_gui_text(c->x, c->y, str, c->c1, c->p); break;
            case GUI_OP_TEXT_STYLED: mini_gui_text_styled(c->x, c->y, str, c->c1, c->p); break;
            case GUI_OP_TEXT_BOX:    mini_gui_text_box(c->x, c->y, c->w, c->h, str, c->c1, c->p, c->b >> 4, c->b & 0xF); break;
            case GUI_OP_SEG7:        mini_gui_seg7(c->x, c->y, c->w, c->h, c->p, str, c->c1, c->c2, c->c3, c->c4); break;
            case GUI_OP_SET_CLIP:    mini_gui_set_clip(c->x, c->y, c->w, c->h); break;
            case GUI_OP_RESET_CLIP:  mini_gui_reset_clip(); break;
            case GUI_OP_CHART:       mini_gui_chart(c->x, c->y, c->w, c->h, (const uint8_t *)str, c->p, c->c1); break;
            case GUI_OP_ARC:         mini_gui_arc(c->x, c->y, c->w, c->h, c->p, (int16_t)c->c2, c->c1); break;
            case GUI_OP_TEXT_SCROLL: mini_gui_text_scroll(c->x, c->y, c->w, c->h, str, c->c1, c->p, (int16_t)c->c2, c->b); break;
            case GUI_OP_BITMAP:      mini_gui_bitmap(c->x, c->y, c->w, c->h, (const uint8_t *)str, c->p, c->c1,
                                                     c->b ? (int)c->c2 : -1); break;
            case GUI_OP_IMAGE:       if (str) mini_gui_image(c->x, c->y, str, c->p, c->c1); break;
            default: break;
        }
    }
}

static int s_dl_last_cmds = 0;

int mini_gui_display_list_size(void) {
    return s_dl_last_cmds;
}

int mini_gui_display_list_capacity(void) {
    return s_dl_cap;
}

size_t mini_gui_display_list_heap_bytes(void) {
    return (s_dl_cmds != s_dl_base_cmds ? (size_t)s_dl_cap * sizeof(GuiCmdW) : 0) +
           (s_dl_strings != s_dl_base_strings ? s_dl_str_cap : 0);
}

/* Calls the render procedure once; returns 1 if it raised an error */
/* Clip save stack for gui-push-clip / gui-pop-clip (no heap allocation) */
#define GUI_CLIP_DEPTH 8
static int s_clip_stack[GUI_CLIP_DEPTH][4];
static int s_clip_depth = 0;

static int gui_call_render(MiniVM *vm, int band_y) {
    s_clip_depth = 0; /* a render that failed half-way must not leak pushes */
    Value fn = gui_lookup_render_proc(vm);
    if (mini_is_void(fn)) return 0;
    mini_call(vm, fn, 0, NULL);
    if (!mini_get_error(vm)) return 0;
    snprintf(s_band_error, sizeof(s_band_error), "band y=%d: %s", band_y, mini_get_error(vm));
    mini_clear_error(vm);
    return 1;
}

/* Rows [y0, y1) as shown, through emit, without touching the panel: BAND
 * rasterizes them again (whole bands, from the display list when it is
 * current), FULL reads the frame. DIRECT keeps no pixels: false. */
bool mini_gui_capture(MiniVM *vm, int y0, int y1, MiniBandEmitFn emit, void *userdata) {
    if (!emit || s_gui_mode == MINI_GUI_MODE_DIRECT) return false;
    if (s_gui_mode == MINI_GUI_MODE_FULL) {
        gui_emit_rows(y0 < 0 ? 0 : y0, y1 > s_height ? s_height : y1, emit, userdata);
        return true;
    }
    mini_gui_band_render(vm, y0, y1, emit, userdata);
    return true;
}

/* Phase 1 of a band render: run the Scheme render once over rows [y0, y1),
 * recording. The clip spans every band so (gui-visible? ...) culls
 * correctly. Skipped when only scrolling text moved inside rows already
 * recorded: its offset is computed while rasterizing, from the frame clock. */
static int gui_record_span(MiniVM *vm, int y0, int y1) {
    int errors = 0;
    bool reuse = s_dl_fresh && !s_rec_overflow && y0 >= s_dl_y0 && y1 <= s_dl_y1;
    if (!reuse) {
        gui_anim_forget(y0, y1); /* recording re-registers the boxes still drawn */
        gui_dl_maybe_shrink();
        s_dl_count = 0;
        s_dl_str_used = 0;
        s_rec_overflow = false;
        gui_set_window(y0, y1);
        s_rec_on = true;
        int failed = gui_call_render(vm, y0);
        s_rec_on = false;
        gui_epoch_sweep();
        errors += failed;
        s_dl_fresh = !failed && !s_rec_overflow;
        s_dl_y0 = y0;
        s_dl_y1 = y1;
        bool small = !s_rec_overflow && s_dl_count <= GUI_DL_BASE_CMDS && s_dl_str_used <= GUI_DL_BASE_STRINGS;
        s_dl_small_runs = small ? s_dl_small_runs + 1 : 0;
    }
    s_dl_last_cmds = s_rec_overflow ? -1 : s_dl_count;
    return errors;
}

/* A moving box redrawn on its own: only its rectangle is rasterized (rows as
 * narrow as the box, in the band buffer) and it goes out in one transfer. The
 * panel never shows the text half moved, as it did when the box rows went
 * out band by band, and far fewer bytes cross the bus. Rows that do not fit
 * the buffer go out in pieces. Returns errors; *rows_out gets the rows. */
static int gui_render_rect(MiniVM *vm, int x0, int y0, int x1, int y1, int *rows_out) {
    int per = s_bits < 8 ? 8 / s_bits : 1;  /* packed pixels: whole bytes */
    x0 -= x0 % per;
    x1 = (x1 + per - 1) / per * per;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > s_width) x1 = s_width;
    if (y1 > s_height) y1 = s_height;
    if (x0 >= x1 || y0 >= y1) return 0;
    size_t stride = gui_row_bytes(x1 - x0, s_bits);
    int fit = (int)(s_surf.stride * (size_t)s_surf.rows / stride); /* the band buffer, as on the board */
    int errors = gui_record_span(vm, y0, y1);
    if (s_rec_overflow || fit < 1) {
        /* no list to replay: the rows the usual way */
        errors += mini_gui_band_render(vm, y0, y1, NULL, NULL);
        if (rows_out) *rows_out += y1 - y0;
        return errors;
    }
    GuiSurface band = s_surf;
    s_in_band_render = true;
    s_frame_ms = s_now_ms;
    for (int ry = y0; ry < y1; ry += fit) {
        int h = (ry + fit > y1) ? (y1 - ry) : fit;
        s_surf.stride = stride;
        s_surf.rows = h;
        s_surf.chunk_rows = h;
        s_surf.y0 = ry;
        s_surf.x0 = x0;
        s_surf.x1 = x1;
        gui_set_window_rect(x0, ry, x1, ry + h);
        mini_gui_clear(MINI_COLOR_BG_DARK);
        gui_dl_replay();
        gui_draw_hold_overlay();
        gui_emit_rect(x0, ry, x1 - x0, h);
        if (rows_out) *rows_out += h;
    }
    s_surf = band;
    s_in_band_render = false;
    s_surf.y0 = 0;
    gui_set_window(0, s_surf.rows);
    return errors;
}

int mini_gui_band_render(MiniVM *vm, int y0, int y1, MiniBandEmitFn emit, void *userdata) {
    if (!vm) return 0;
    if (y0 < 0) y0 = 0;
    if (y1 > s_height) y1 = s_height;
    if (y0 >= y1) return 0;
    int errors = 0;
    s_band_error[0] = '\0';
    s_active_sub_buffer = NULL;
    s_target_pixels = NULL;
    s_frame_ms = s_now_ms;  /* every band of this frame scrolls to the same offset */

    if (s_gui_mode != MINI_GUI_MODE_BAND) {
        /* FULL and DIRECT: one pass over the span, then FULL sends it out */
        gui_anim_forget(y0, y1); /* the pass re-registers the boxes it still draws */
        s_in_band_render = true;
        gui_set_window(y0, y1);
        mini_gui_clear(MINI_COLOR_BG_DARK);
        errors += gui_call_render(vm, y0);
        gui_epoch_sweep();
        gui_draw_hold_overlay();
        s_in_band_render = false;
        gui_set_window(0, s_height);
        if (s_gui_mode == MINI_GUI_MODE_FULL) gui_emit_rows(y0, y1, emit, userdata);
        s_dl_last_cmds = 0;
        return errors;
    }

    int band_h = s_surf.rows;
    if (band_h <= 0) return 0;
    int first_band = (y0 / band_h) * band_h;
    int last_row = ((y1 + band_h - 1) / band_h) * band_h;
    if (last_row > s_height) last_row = s_height;
    s_in_band_render = true;

    errors += gui_record_span(vm, first_band, last_row);

    /* Phase 2: rasterize each band in C (or re-run Scheme if recording failed) */
    for (int band_y = first_band; band_y < y1; band_y += band_h) {
        int actual_h = (band_y + band_h > s_height) ? (s_height - band_y) : band_h;
        s_surf.y0 = band_y;
        gui_set_window(band_y, band_y + actual_h);
        mini_gui_clear(MINI_COLOR_BG_DARK);
        if (s_rec_overflow) {
            errors += gui_call_render(vm, band_y);
        } else {
            gui_dl_replay();
        }
        gui_draw_hold_overlay();
        gui_emit_rows(band_y, band_y + actual_h, emit, userdata);
    }
    s_in_band_render = false;

    /* Park on band 0 so stray immediate-mode draws stay harmless */
    s_surf.y0 = 0;
    gui_set_window(0, band_h);
    return errors;
}

int mini_gui_service(MiniVM *vm, int *rows_out) {
    int y0 = 0, y1 = 0, rows = 0, errors = 0;
    while (mini_gui_take_invalidated(&y0, &y1)) {
        errors += mini_gui_band_render(vm, y0, y1, NULL, NULL);
        rows += y1 - y0;
    }
    /* Moving boxes that are due, each on its own (taken first: a recording
     * pass re-registers the boxes and reorders the table) */
    GuiAnim due[GUI_ANIM_MAX];
    int n_due = 0;
    for (int i = 0; i < s_anim_n; i++) {
        if (!s_anim[i].due) continue;
        s_anim[i].due = false;
        due[n_due++] = s_anim[i];
    }
    for (int i = 0; i < n_due; i++) {
        if (gui_rect_output_ok()) errors += gui_render_rect(vm, due[i].x0, due[i].y0, due[i].x1, due[i].y1, &rows);
        else { errors += mini_gui_band_render(vm, due[i].y0, due[i].y1, NULL, NULL); rows += due[i].y1 - due[i].y0; }
    }
    if (s_gui_mode == MINI_GUI_MODE_FULL && s_flush_y1 > s_flush_y0) {
        rows += s_flush_y1 - s_flush_y0;
        gui_emit_rows(s_flush_y0, s_flush_y1, NULL, NULL);
        s_flush_y0 = s_flush_y1 = 0;
    }
    if (rows_out) *rows_out = rows;
    return errors;
}

/* Fills the effective clip: the whole screen, the current band, or the
 * span being re-rendered */
void mini_gui_clear(uint16_t color) {
    GUI_REC(GUI_OP_CLEAR, 0, 0, 0, 0, 0, 0, color, 0, 0, 0, NULL);
    gui_fill_clipped(s_clip_x0, s_clip_y0, s_clip_x1, s_clip_y1, color);
}

void mini_gui_flush(void) {
    mini_gui_mark_dirty_rect(0, 0, s_width, s_height);
}

uint16_t *mini_gui_get_vfb(void) {
    return s_vfb;
}

int mini_gui_get_width(void) {
    return s_width;
}

int mini_gui_get_height(void) {
    return s_height;
}

bool mini_gui_is_dirty(void) {
    return s_dirty;
}

/* ========================================================================= */
/* Sub-Framebuffer & Offscreen Component Buffers                             */
/* ========================================================================= */

MiniSubBuffer *mini_gui_create_buffer(int w, int h) {
    if (w <= 0 || h <= 0) return NULL;
    MiniSubBuffer *buf = (MiniSubBuffer *)malloc(sizeof(MiniSubBuffer));
    if (!buf) return NULL;
    buf->width = w;
    buf->height = h;
    buf->pixels = (uint16_t *)calloc(w * h, sizeof(uint16_t));
    if (!buf->pixels) {
        free(buf);
        return NULL;
    }
    return buf;
}

void mini_gui_destroy_buffer(MiniSubBuffer *buf) {
    if (!buf) return;
    if (s_active_sub_buffer == buf) {
        mini_gui_reset_target();
    }
    if (buf->pixels) free(buf->pixels);
    free(buf);
}

void mini_gui_set_target(MiniSubBuffer *buf) {
    if (s_rec_on) { s_rec_overflow = true; return; }

    if (buf && buf->pixels) {
        s_active_sub_buffer = buf;
        s_target_pixels = buf->pixels;
        s_target_width = buf->width;
        s_target_height = buf->height;
        mini_gui_reset_clip();
    }
}

void mini_gui_reset_target(void) {
    s_active_sub_buffer = NULL;
    s_target_pixels = NULL;
    s_target_width = s_width;
    s_target_height = s_height;
    mini_gui_reset_clip();
}

void mini_gui_blit_buffer(int dest_x, int dest_y, const MiniSubBuffer *buf) {
    if (s_rec_on) { s_rec_overflow = true; return; }

    if (!buf || !buf->pixels) return;
    int src_w = buf->width;
    int src_h = buf->height;

    int x0 = dest_x < s_clip_x0 ? s_clip_x0 : dest_x;
    int y0 = dest_y < s_clip_y0 ? s_clip_y0 : dest_y;
    int x1 = (dest_x + src_w) > s_clip_x1 ? s_clip_x1 : (dest_x + src_w);
    int y1 = (dest_y + src_h) > s_clip_y1 ? s_clip_y1 : (dest_y + src_h);
    if (x0 >= x1 || y0 >= y1) return;

    int copy_w = x1 - x0;
    for (int y = y0; y < y1; y++) {
        const uint16_t *src_row = &buf->pixels[(y - dest_y) * src_w + (x0 - dest_x)];
        if (s_active_sub_buffer) {
            memcpy(&s_target_pixels[y * s_target_width + x0], src_row, (size_t)copy_w * sizeof(uint16_t));
            continue;
        }
        /* Runs of equal color keep DIRECT mode to one fill per run */
        int run = 0;
        for (int x = 1; x <= copy_w; x++) {
            if (x == copy_w || src_row[x] != src_row[run]) {
                gui_fill_clipped(x0 + run, y, x0 + x, y + 1, src_row[run]);
                run = x;
            }
        }
    }
}

/* ========================================================================= */
/* Core Drawing Primitives with Clipping                                     */
/* ========================================================================= */

void mini_gui_pixel(int x, int y, uint16_t color) {
    GUI_REC(GUI_OP_PIXEL, x, y, 0, 0, 0, 0, color, 0, 0, 0, NULL);
    if (x < s_clip_x0 || x >= s_clip_x1 || y < s_clip_y0 || y >= s_clip_y1) return;
    if (!s_active_sub_buffer && s_gui_mode == MINI_GUI_MODE_BAND && s_bits == 16 && s_pal_n == 0 &&
        s_scheme != MINI_GUI_SCHEME_INVERT) {
        /* Hot path of lines and arcs in the default configuration */
        uint8_t *row = surf_row(y);
        if (row) ((uint16_t *)(void *)row)[x - s_surf.x0] = color;
        return;
    }
    gui_fill_clipped(x, y, x + 1, y + 1, color);
}

uint16_t mini_gui_get_pixel(int x, int y) {
    if (s_active_sub_buffer) {
        if (x < 0 || y < 0 || x >= s_target_width || y >= s_target_height) return 0;
        return s_target_pixels[y * s_target_width + x];
    }
    if (x < 0 || y < 0 || x >= s_width || y >= s_height) return 0;
    const uint8_t *row = s_gui_mode == MINI_GUI_MODE_DIRECT ? NULL : surf_row(y);
    if (row && x >= s_surf.x0 && (s_surf.x1 == 0 || x < s_surf.x1)) {
        int sx = x - s_surf.x0;
        if (s_bits == 16) return ((const uint16_t *)(const void *)row)[sx];
        return s_palette[s_bits == 8 ? row[sx] : get_packed(row, sx, s_bits)];
    }
    return s_vfb ? s_vfb[(size_t)y * (size_t)s_width + x] : 0;
}

void mini_gui_line(int x0, int y0, int x1, int y1, uint16_t color) {
    GUI_REC(GUI_OP_LINE, x0, y0, x1, y1, 0, 0, color, 0, 0, 0, NULL);
    if (GUI_ROWS_HIDDEN(y0 < y1 ? y0 : y1, (y0 < y1 ? y1 - y0 : y0 - y1) + 1)) return;

    int dx = abs(x1 - x0);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0);
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    while (1) {
        mini_gui_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* A band draws only the bitmap's rows inside it, each row's runs of set
 * dots as single rectangles */
void mini_gui_bitmap(int x, int y, int w, int h, const uint8_t *bits, int scale, uint16_t fg, int bg) {
    if (!bits || w <= 0 || h <= 0 || scale <= 0) return;
    if (s_rec_on) {
        gui_rec_bitmap(x, y, w, h, bits, scale, fg, bg);
        return;
    }
    if (GUI_ROWS_HIDDEN(y, h * scale)) return;
    if (bg >= 0) mini_gui_rect(x, y, w * scale, h * scale, (uint16_t)bg, true);
    int stride = (w + 7) / 8;
    int r0 = s_clip_y0 > y ? (s_clip_y0 - y) / scale : 0;
    int r1 = (s_clip_y1 - 1 - y) / scale;
    if (r1 >= h) r1 = h - 1;
    for (int r = r0; r <= r1; r++) {
        const uint8_t *row = bits + (size_t)r * (size_t)stride;
        int c = 0;
        while (c < w) {
            while (c < w && !(row[c >> 3] & (0x80 >> (c & 7)))) c++;
            int start = c;
            while (c < w && (row[c >> 3] & (0x80 >> (c & 7)))) c++;
            if (c > start) mini_gui_rect(x + start * scale, y + r * scale, (c - start) * scale, scale, fg, true);
        }
    }
}

/* ========================================================================= */
/* Images (minischeme_image)                                                 */
/* ========================================================================= */
/* One image kept open (its tables, about 6 KB): the bands of a frame and the
 * frames of an animation come from the same file. */
static MiniImage *s_img = NULL;
static char s_img_path[112];

void mini_gui_image_forget(void) {
    mini_image_close(s_img);
    s_img = NULL;
    s_img_path[0] = '\0';
}

static MiniImage *gui_image_get(const char *path, char *err, size_t cap) {
    if (s_img && strcmp(s_img_path, path) == 0) return s_img;
    mini_gui_image_forget();
    if (strlen(path) >= sizeof(s_img_path)) {
        snprintf(err, cap, "path too long");
        return NULL;
    }
    s_img = mini_image_open(path, err, cap);
    if (s_img) snprintf(s_img_path, sizeof(s_img_path), "%s", path);
    return s_img;
}

/* A frame's delay at speed percent; a GIF's 0 (or 10 ms) plays as 100 ms, as browsers do */
static uint32_t gui_image_delay(const MiniImage *im, int f, int speed) {
    uint32_t d = mini_image_delay(im, f);
    if (d <= 10) d = 100;
    d = d * 100u / (uint32_t)speed;
    return d < 20 ? 20 : d;
}

/* The frame showing `age` ms into the animation; *shortest the shortest delay */
static int gui_image_frame(const MiniImage *im, uint32_t age, int speed, uint32_t *shortest) {
    int n = mini_image_frames(im);
    uint64_t total = 0;
    *shortest = 0xFFFFFFFFu;
    for (int i = 0; i < n; i++) {
        uint32_t d = gui_image_delay(im, i, speed);
        total += d;
        if (d < *shortest) *shortest = d;
    }
    uint64_t t = total ? age % total : 0;
    for (int i = 0; i < n; i++) {
        uint32_t d = gui_image_delay(im, i, speed);
        if (t < d) return i;
        t -= d;
    }
    return n - 1;
}

typedef struct { int dx, dy; } GuiImagePut;

/* Decoded pixels to the surface (already clipped) */
static void gui_image_put(void *ud, int x, int y, const uint16_t *px, int n) {
    const GuiImagePut *o = (const GuiImagePut *)ud;
    int sx = x + o->dx, sy = y + o->dy;
    if (!s_active_sub_buffer && s_gui_mode == MINI_GUI_MODE_BAND && s_bits == 16 && s_pal_n == 0 &&
        s_scheme != MINI_GUI_SCHEME_INVERT) {
        uint8_t *row = surf_row(sy);
        if (row) memcpy((uint16_t *)(void *)row + (sx - s_surf.x0), px, (size_t)n * 2);
        return;
    }
    for (int i = 0; i < n; i++) gui_fill_clipped(sx + i, sy, sx + i + 1, sy + 1, px[i]);
}

void mini_gui_image(int x, int y, const char *path, int frame, int speed) {
    char err[96];
    MiniImage *im = path ? gui_image_get(path, err, sizeof(err)) : NULL;
    if (!im) return;
    int w = mini_image_width(im), h = mini_image_height(im), n = mini_image_frames(im);
    if (speed <= 0) speed = 100;
    if (s_rec_on) {
        gui_rec(GUI_OP_IMAGE, x, y, w, h, frame, 0, (uint16_t)speed, 0, 0, 0, path);
    } else if (GUI_ROWS_HIDDEN(y, h)) {
        return;   /* other bands: the recording pass registered the animation */
    }
    int f = frame >= 0 ? (frame < n ? frame : n - 1) : 0;
    if (frame < 0 && n > 1) {
        /* an animation: its own clock from when it appeared; the box redrawn
         * on its own at the shortest frame delay */
        uint32_t born = gui_epoch_start(path, x, y, w, h, 0x494D), shortest;
        f = gui_image_frame(im, s_frame_ms - born, speed, &shortest);
        int bx0 = x > s_uclip_x0 ? x : s_uclip_x0, by0 = y > s_uclip_y0 ? y : s_uclip_y0;
        int bx1 = x + w < s_uclip_x1 ? x + w : s_uclip_x1, by1 = y + h < s_uclip_y1 ? y + h : s_uclip_y1;
        if (bx0 < 0) bx0 = 0;
        if (by0 < 0) by0 = 0;
        if (bx1 > s_width) bx1 = s_width;
        if (by1 > s_height) by1 = s_height;
        if (!s_active_sub_buffer && bx0 < bx1 && by0 < by1) gui_anim_add(bx0, by0, bx1, by1, shortest);
    }
    if (s_rec_on) return;
    int cx0 = x > s_clip_x0 ? x : s_clip_x0, cx1 = x + w < s_clip_x1 ? x + w : s_clip_x1;
    int cy0 = y > s_clip_y0 ? y : s_clip_y0, cy1 = y + h < s_clip_y1 ? y + h : s_clip_y1;
    if (cx0 >= cx1 || cy0 >= cy1) return;
    GuiImagePut o = { x, y };
    if (mini_image_draw(im, f, cx0 - x, cx1 - x, cy0 - y, cy1 - y, gui_image_put, &o, err, sizeof(err)) != 0) {
        mini_gui_image_forget();   /* a broken or replaced file: opened again next time */
    }
    s_dirty = true;
}

/* Polyline through n points spread over w; ys[i] is the row offset in [0, h) */
void mini_gui_chart(int x, int y, int w, int h, const uint8_t *ys, int n, uint16_t color) {
    if (!ys || n < 2 || w < 2 || h < 1) return;
    if (s_rec_on) { gui_rec_bytes(GUI_OP_CHART, x, y, w, h, n, color, ys); return; }
    if (GUI_ROWS_HIDDEN(y, h)) return;
    /* x only grows, so each column is one vertical span from the line's y at
     * the column to its y at the next column; spans are clipped to the band
     * first, so a band costs O(columns) instead of walking every segment */
    int band_lo = s_clip_y0, band_hi = s_clip_y1 - 1;
    int xa = x, ya = y + ys[0];
    for (int i = 1; i < n; i++) {
        int xb = x + (int)((long)i * (w - 1) / (n - 1));
        int yb = y + ys[i];
        int lo_seg = ya < yb ? ya : yb, hi_seg = ya < yb ? yb : ya;
        if (hi_seg >= band_lo && lo_seg <= band_hi) {
            int dx = xb - xa;
            for (int cx = xa; cx < xb || (dx == 0 && cx == xa); cx++) {
                int y_from = dx ? ya + (yb - ya) * (cx - xa) / dx : ya;
                int y_to = dx ? ya + (yb - ya) * (cx + 1 - xa) / dx : yb;
                int s0 = y_from < y_to ? y_from : y_to, s1 = y_from < y_to ? y_to : y_from;
                if (s0 < band_lo) s0 = band_lo;
                if (s1 > band_hi) s1 = band_hi;
                if (s0 <= s1) mini_gui_rect(cx, s0, 1, s1 - s0 + 1, color, true);
                if (dx == 0) break;
            }
        }
        xa = xb;
        ya = yb;
    }
    if (ya >= band_lo && ya <= band_hi) mini_gui_pixel(xa, ya, color); /* last point */
}

/* Ring of the given thickness from start_deg sweeping sweep_deg clockwise
 * (0 = 3 o'clock, screen y down). Sector test with cross products: one
 * sin/cos pair per arc, none per pixel. */
void mini_gui_arc(int cx, int cy, int r, int thick, int start_deg, int sweep_deg, uint16_t color) {
    if (r <= 0 || thick <= 0 || sweep_deg <= 0) return;
    if (s_rec_on) { gui_rec(GUI_OP_ARC, cx, cy, r, thick, start_deg, 0, color, (uint16_t)sweep_deg, 0, 0, NULL); return; }
    if (GUI_ROWS_HIDDEN(cy - r, 2 * r + 1)) return;
    if (sweep_deg > 360) sweep_deg = 360;
    const double rad = 3.14159265358979 / 180.0;
    float sx = (float)cos(start_deg * rad), sy = (float)sin(start_deg * rad);
    float ex = (float)cos((start_deg + sweep_deg) * rad), ey = (float)sin((start_deg + sweep_deg) * rad);
    int ro2 = r * r, ri = r - thick, ri2 = ri > 0 ? ri * ri : 0;
    int y0 = cy - r < s_clip_y0 ? s_clip_y0 : cy - r;
    int y1 = cy + r >= s_clip_y1 ? s_clip_y1 - 1 : cy + r;
    for (int py = y0; py <= y1; py++) {
        int dy = py - cy;
        for (int dx = -r; dx <= r; dx++) {
            int d2 = dx * dx + dy * dy;
            if (d2 > ro2 || d2 < ri2) continue;
            bool in;
            if (sweep_deg >= 360) {
                in = true;
            } else {
                float c_s = sx * (float)dy - sy * (float)dx;  /* >= 0: clockwise of start */
                float c_e = ex * (float)dy - ey * (float)dx;  /* <= 0: before the end */
                in = (sweep_deg <= 180) ? (c_s >= 0 && c_e <= 0) : (c_s >= 0 || c_e <= 0);
            }
            if (in) mini_gui_pixel(cx + dx, py, color);
        }
    }
}

void mini_gui_rect(int x, int y, int w, int h, uint16_t color, bool filled) {
    GUI_REC(GUI_OP_RECT, x, y, w, h, 0, filled ? 1 : 0, color, 0, 0, 0, NULL);

    if (w <= 0 || h <= 0) return;
    if (filled) {
        int x0 = x < s_clip_x0 ? s_clip_x0 : x;
        int y0 = y < s_clip_y0 ? s_clip_y0 : y;
        int x1 = (x + w) > s_clip_x1 ? s_clip_x1 : (x + w);
        int y1 = (y + h) > s_clip_y1 ? s_clip_y1 : (y + h);
        if (x0 >= x1 || y0 >= y1) return;

        gui_fill_clipped(x0, y0, x1, y1, color);
    } else {
        mini_gui_line(x, y, x + w - 1, y, color);
        mini_gui_line(x, y + h - 1, x + w - 1, y + h - 1, color);
        mini_gui_line(x, y, x, y + h - 1, color);
        mini_gui_line(x + w - 1, y, x + w - 1, y + h - 1, color);
    }
}

static void draw_circle_helper(int cx, int cy, int r, int corners, uint16_t color) {
    int f = 1 - r;
    int ddF_x = 1;
    int ddF_y = -2 * r;
    int x = 0;
    int y = r;

    while (x <= y) {
        if (corners & 0x1) {
            mini_gui_pixel(cx + x, cy - y, color);
            mini_gui_pixel(cx + y, cy - x, color);
        }
        if (corners & 0x2) {
            mini_gui_pixel(cx + x, cy + y, color);
            mini_gui_pixel(cx + y, cy + x, color);
        }
        if (corners & 0x4) {
            mini_gui_pixel(cx - y, cy + x, color);
            mini_gui_pixel(cx - x, cy + y, color);
        }
        if (corners & 0x8) {
            mini_gui_pixel(cx - y, cy - x, color);
            mini_gui_pixel(cx - x, cy - y, color);
        }
        if (f >= 0) {
            y--;
            ddF_y += 2;
            f += ddF_y;
        }
        x++;
        ddF_x += 2;
        f += ddF_x;
    }
}

void mini_gui_round_rect(int x, int y, int w, int h, int r, uint16_t color, bool filled) {
    GUI_REC(GUI_OP_RRECT, x, y, w, h, r, filled ? 1 : 0, color, 0, 0, 0, NULL);
    if (GUI_ROWS_HIDDEN(y, h)) return;

    if (w <= 0 || h <= 0) return;
    if (r <= 0) {
        mini_gui_rect(x, y, w, h, color, filled);
        return;
    }
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;

    if (filled) {
        for (int dy = 0; dy < h; dy++) {
            int inset = 0;
            if (dy < r) {
                int off = r - dy;
                int inner = (int)__builtin_sqrt((double)(r * r - off * off));
                inset = r - inner;
            } else if (dy >= h - r) {
                int off = dy - (h - 1 - r);
                int inner = (int)__builtin_sqrt((double)(r * r - off * off));
                inset = r - inner;
            }
            int row_x = x + inset;
            int row_w = w - 2 * inset;
            if (row_w > 0) {
                mini_gui_rect(row_x, y + dy, row_w, 1, color, true);
            }
        }
    } else {
        mini_gui_line(x + r, y, x + w - r - 1, y, color);
        mini_gui_line(x + r, y + h - 1, x + w - r - 1, y + h - 1, color);
        mini_gui_line(x, y + r, x, y + h - r - 1, color);
        mini_gui_line(x + w - 1, y + r, x + w - 1, y + h - r - 1, color);

        draw_circle_helper(x + r, y + r, r, 0x8, color);
        draw_circle_helper(x + w - r - 1, y + r, r, 0x1, color);
        draw_circle_helper(x + w - r - 1, y + h - r - 1, r, 0x2, color);
        draw_circle_helper(x + r, y + h - r - 1, r, 0x4, color);
    }
}

void mini_gui_card(int x, int y, int w, int h, int r, uint16_t bg_col, uint16_t border_col) {
    GUI_REC(GUI_OP_CARD, x, y, w, h, r, 0, bg_col, border_col, 0, 0, NULL);
    if (GUI_ROWS_HIDDEN(y, h)) return;

    if (w <= 0 || h <= 0) return;
    if (r > 0) {
        mini_gui_round_rect(x, y, w, h, r, bg_col, true);
        if (border_col != 0 && border_col != bg_col) {
            mini_gui_round_rect(x, y, w, h, r, border_col, false);
        }
    } else {
        mini_gui_rect(x, y, w, h, bg_col, true);
        if (border_col != 0 && border_col != bg_col) {
            mini_gui_rect(x, y, w, h, border_col, false);
        }
    }
}
void mini_gui_circle(int cx, int cy, int r, uint16_t color, bool filled) {
    GUI_REC(GUI_OP_CIRCLE, cx, cy, 0, 0, r, filled ? 1 : 0, color, 0, 0, 0, NULL);
    if (GUI_ROWS_HIDDEN(cy - r, 2 * r + 1)) return;

    if (r <= 0) {
        mini_gui_pixel(cx, cy, color);
        return;
    }
    if (filled) {
        for (int y = -r; y <= r; y++) {
            int dx = (int)__builtin_sqrt(r * r - y * y);
            mini_gui_line(cx - dx, cy + y, cx + dx, cy + y, color);
        }
    } else {
        int x = 0;
        int y = r;
        int d = 3 - 2 * r;
        while (y >= x) {
            mini_gui_pixel(cx + x, cy + y, color);
            mini_gui_pixel(cx - x, cy + y, color);
            mini_gui_pixel(cx + x, cy - y, color);
            mini_gui_pixel(cx - x, cy - y, color);
            mini_gui_pixel(cx + y, cy + x, color);
            mini_gui_pixel(cx - y, cy + x, color);
            mini_gui_pixel(cx + y, cy - x, color);
            mini_gui_pixel(cx - y, cy - x, color);
            x++;
            if (d > 0) {
                y--;
                d = d + 4 * (x - y) + 10;
            } else {
                d = d + 4 * x + 6;
            }
        }
    }
}

void mini_gui_text(int x, int y, const char *str, uint16_t color, int size) {
    GUI_REC(GUI_OP_TEXT, x, y, 0, 0, size, 0, color, 0, 0, 0, str ? str : "");

    if (!str || size <= 0) return;
    int cur_x = x;
    int cur_y = y;

    for (const unsigned char *p = (const unsigned char *)str; *p; p++) {
        unsigned char c = *p;
        if (c == '\n') {
            cur_x = x;
            cur_y += 18 * size;
            continue;
        }

        /* 4-byte UTF-8 (e.g. Emoji) - gracefully skip */
        if ((c & 0xF8) == 0xF0) {
            if (*(p + 1) && *(p + 2) && *(p + 3)) {
                p += 3;
            }
            continue;
        }

        /* 3-byte UTF-8 (CJK Ideographs / Fullwidth punctuation) */
        if ((c & 0xF0) == 0xE0) {
            unsigned char c1 = *(p + 1);
            unsigned char c2 = c1 ? *(p + 2) : 0;
            if (c1 && c2) {
                p += 2;
                uint16_t cp = (uint16_t)(((c & 0x0F) << 12) | ((c1 & 0x3F) << 6) | (c2 & 0x3F));
                const uint8_t *bm = cjk_get_glyph16(cp);
                if (bm) {
                    for (int row = 0; row < 16; row++) {
                        uint8_t byte0 = bm[row * 2];
                        uint8_t byte1 = bm[row * 2 + 1];
                        for (int col = 0; col < 8; col++) {
                            if (byte0 & (0x80 >> col)) {
                                if (size == 1) mini_gui_pixel(cur_x + col, cur_y + row, color);
                                else mini_gui_rect(cur_x + col * size, cur_y + row * size, size, size, color, true);
                            }
                            if (byte1 & (0x80 >> col)) {
                                if (size == 1) mini_gui_pixel(cur_x + 8 + col, cur_y + row, color);
                                else mini_gui_rect(cur_x + (8 + col) * size, cur_y + row * size, size, size, color, true);
                            }
                        }
                    }
                    cur_x += 16 * size;
                    continue;
                } else {
                    /* Placeholder box for unknown CJK glyph */
                    mini_gui_rect(cur_x, cur_y, 14 * size, 14 * size, color, false);
                    cur_x += 16 * size;
                    continue;
                }
            }
        }

        /* 2-byte UTF-8 (e.g. ° degree symbol 0xC2 0xB0) */
        if ((c & 0xE0) == 0xC0) {
            unsigned char c1 = *(p + 1);
            if (c1) {
                p++;
                uint16_t cp = (uint16_t)(((c & 0x1F) << 6) | (c1 & 0x3F));
                if (cp == 0xB0) {
                    c = 127; /* degree symbol in s_font8x8 */
                } else {
                    c = '?';
                }
            }
        }

        /* 1-byte ASCII glyph */
        int idx = 0;
        if (c >= 32 && c <= 126) {
            idx = c - 32;
        } else if (c == 127) {
            idx = 95; /* degree glyph */
        } else {
            idx = '?' - 32;
        }

        const uint8_t *bitmap = s_font8x8[idx];
        /* If size == 1, offset by +4 to vertically baseline-align with 16px CJK */
        int y_off = (size == 1) ? 4 : 0;
        for (int row = 0; row < 8; row++) {
            uint8_t line = bitmap[row];
            for (int col = 0; col < 8; col++) {
                if (line & (0x80 >> col)) {
                    if (size == 1) {
                        mini_gui_pixel(cur_x + col, cur_y + y_off + row, color);
                    } else {
                        mini_gui_rect(cur_x + col * size, cur_y + row * size, size, size, color, true);
                    }
                }
            }
        }
        cur_x += 8 * size;
    }
}

/* ========================================================================= */
/* Font Selection, Measurement & Styled Box Alignment                        */
/* ========================================================================= */

static int s_current_font = MINI_FONT_SYSTEM_16;

void mini_gui_set_font(int font_id) {
    s_current_font = font_id;
}

int mini_gui_get_font(void) {
    return s_current_font;
}

int mini_gui_text_width(const char *str, int font_id) {
    return mini_font_text_width(font_id, str);
}

/* Glyph rows become runs of set bits: one fill per run in every mode */
/* Blends color over the pixel at x y with coverage v of max (the pixel is
 * read back from the surface; DIRECT mode keeps none: half coverage or
 * more draws the color) */
static void blend_pixel(int x, int y, uint16_t color, unsigned v, unsigned max) {
    if (x < s_clip_x0 || x >= s_clip_x1) return;
    if (s_gui_mode == MINI_GUI_MODE_DIRECT && !s_active_sub_buffer) {
        if (v * 2 >= max) gui_hrun(x, x + 1, y, color);
        return;
    }
    uint16_t bg = mini_gui_get_pixel(x, y);
    if (s_scheme == MINI_GUI_SCHEME_INVERT && !s_active_sub_buffer) bg ^= 0xFFFF; /* back to a drawing color */
    unsigned r = ((color >> 11) * v + (bg >> 11) * (max - v) + max / 2) / max;
    unsigned g = (((color >> 5) & 0x3F) * v + ((bg >> 5) & 0x3F) * (max - v) + max / 2) / max;
    unsigned b = ((color & 0x1F) * v + (bg & 0x1F) * (max - v) + max / 2) / max;
    gui_hrun(x, x + 1, y, (uint16_t)((r << 11) | (g << 5) | b));
}

/* Glyph with 2 or 4 bits of coverage per pixel: full runs are filled,
 * edge pixels blended */
static void draw_glyph_aa(const MiniGlyph *g, int cur_x, int cur_y, int glyph_h, uint16_t color) {
    int bpp = g->bpp, bpr = g->bytes_per_row;
    unsigned max = (1u << bpp) - 1u;
    int width = bpr * 8 / bpp;
    int r0 = s_clip_y0 - cur_y, r1 = s_clip_y1 - cur_y;
    if (r0 < 0) r0 = 0;
    if (r1 > glyph_h) r1 = glyph_h;
    for (int r = r0; r < r1; r++) {
        const uint8_t *src = &g->bitmap[r * bpr];
        int run = -1;
        for (int c = 0; c <= width; c++) {
            unsigned v = 0;
            if (c < width) {
                int bit = c * bpp;
                v = (src[bit >> 3] >> (8 - bpp - (bit & 7))) & max;
            }
            if (v == max) {
                if (run < 0) run = c;
                continue;
            }
            if (run >= 0) {
                gui_hrun(cur_x + run, cur_x + c, cur_y + r, color);
                run = -1;
            }
            if (v) blend_pixel(cur_x + c, cur_y + r, color, v, max);
        }
    }
}

static inline void draw_glyph_fast(const MiniGlyph *g, int cur_x, int cur_y, int glyph_h, uint16_t color) {
    if (!g) return;
    if (g->bpp > 1) {
        draw_glyph_aa(g, cur_x, cur_y, glyph_h, color);
        return;
    }
    int bpr = g->bytes_per_row;
    int r0 = s_clip_y0 - cur_y, r1 = s_clip_y1 - cur_y;
    if (r0 < 0) r0 = 0;
    if (r1 > glyph_h) r1 = glyph_h;
    for (int r = r0; r < r1; r++) {
        const uint8_t *src = &g->bitmap[r * bpr];
        int run = -1;
        for (int c = 0; c <= bpr * 8; c++) {
            bool on = c < bpr * 8 && (src[c >> 3] & (0x80 >> (c & 7)));
            if (on && run < 0) run = c;
            if (!on && run >= 0) {
                gui_hrun(cur_x + run, cur_x + c, cur_y + r, color);
                run = -1;
            }
        }
    }
}

void mini_gui_text_styled(int x, int y, const char *str, uint16_t color, int font_id) {
    GUI_REC(GUI_OP_TEXT_STYLED, x, y, 0, 0, font_id, 0, color, 0, 0, 0, str ? str : "");

    if (!str) return;
    const MiniFont *font = mini_font_get(font_id);
    if (!font) font = mini_font_get(s_current_font);
    if (!font) return;
    /* Single-line text entirely outside the clip (other bands): nothing to do */
    if (!strchr(str, '\n') && (y >= s_clip_y1 || y + font->height <= s_clip_y0)) return;

    int cur_x = x;
    int cur_y = y;

    for (const unsigned char *p = (const unsigned char *)str; *p; p++) {
        unsigned char c = *p;
        if (c == '\n') {
            cur_x = x;
            cur_y += font->height + 2;
            continue;
        }

        uint16_t cp = c;
        if ((c & 0xF8) == 0xF0) {
            if (*(p + 1) && *(p + 2) && *(p + 3)) p += 3;
            continue;
        } else if ((c & 0xF0) == 0xE0) {
            unsigned char c1 = *(p + 1);
            unsigned char c2 = c1 ? *(p + 2) : 0;
            if (c1 && c2) {
                p += 2;
                cp = (uint16_t)(((c & 0x0F) << 12) | ((c1 & 0x3F) << 6) | (c2 & 0x3F));
            }
        } else if ((c & 0xE0) == 0xC0) {
            unsigned char c1 = *(p + 1);
            if (c1) {
                p++;
                cp = (uint16_t)(((c & 0x1F) << 6) | (c1 & 0x3F));
            }
        }

        /* Every source of the font's chain, then the system font (font 0,
         * maybe replaced in config.ini), then the built-in one. A glyph of
         * another font sits on the same baseline when both fonts know
         * theirs, else it is centred vertically. */
        const MiniFont *used = font;
        const MiniGlyph *g = mini_font_glyph(font, cp, &used);
        if (g) {
            int y_offset = 0;
            if (used != font) {
                if (font->ascent && used->ascent) y_offset = (int)font->ascent - (int)used->ascent;
                else if (font->height > used->height) y_offset = (font->height - used->height) / 2;
            }
            /* Glyphs left or right of the clip cost nothing (scrolling text) */
            int glyph_w = g->bytes_per_row * 8 / (g->bpp > 1 ? g->bpp : 1);
            if (cur_x < s_clip_x1 && cur_x + glyph_w > s_clip_x0) {
                draw_glyph_fast(g, cur_x, cur_y + y_offset, used->height, color);
            }
            cur_x += g->advance;
        } else {
            int pw = (cp < 128) ? 8 : 16;
            mini_gui_rect(cur_x, cur_y, pw - 2, font->height - 2, color, false);
            cur_x += pw;
        }
    }
}

void mini_gui_text_box(int x, int y, int w, int h, const char *str, uint16_t color, int font_id, int halign, int valign) {
    GUI_REC(GUI_OP_TEXT_BOX, x, y, w, h, font_id, (uint8_t)(((halign & 0xF) << 4) | (valign & 0xF)), color, 0, 0, 0, str ? str : "");
    if (GUI_ROWS_HIDDEN(y, h)) return; /* text is clipped to the box */

    if (!str || w <= 0 || h <= 0) return;
    int tw = mini_font_text_width(font_id, str);
    const MiniFont *f = mini_font_get(font_id);
    int th = f ? f->height : 16;

    int dx = x;
    if (halign == 1) { /* Center */
        dx = x + (w - tw) / 2;
    } else if (halign == 2) { /* Right */
        dx = x + w - tw;
    }

    int dy = y;
    if (valign == 1) { /* Middle */
        dy = y + (h - th) / 2;
    } else if (valign == 2) { /* Bottom */
        dy = y + h - th;
    }

    int old_x0 = s_uclip_x0, old_y0 = s_uclip_y0, old_x1 = s_uclip_x1, old_y1 = s_uclip_y1;
    /* Nest inside the caller's clip rather than replacing it */
    int bx0 = x > old_x0 ? x : old_x0;
    int by0 = y > old_y0 ? y : old_y0;
    int bx1 = (x + w) < old_x1 ? (x + w) : old_x1;
    int by1 = (y + h) < old_y1 ? (y + h) : old_y1;
    mini_gui_set_clip(bx0, by0, bx1 - bx0, by1 - by0);
    mini_gui_text_styled(dx, dy, str, color, font_id);
    s_uclip_x0 = old_x0; s_uclip_y0 = old_y0; s_uclip_x1 = old_x1; s_uclip_y1 = old_y1;
    gui_apply_clip();
}

/* ========================================================================= */
/* Scrolling Text (marquee and vertical scroller)                            */
/* ========================================================================= */
/* Text longer than its box scrolls in a loop. The offset comes from the
 * frame clock, so a box needs no state: the render procedure just draws it
 * every frame, and mini_gui_animate() re-invalidates the box rows while it
 * moves. Text that fits is drawn still. */


static void gui_anim_add(int x0, int y0, int x1, int y1, uint32_t period_ms) {
    for (int i = 0; i < s_anim_n; i++) {
        GuiAnim *a = &s_anim[i];
        if (a->x0 == x0 && a->y0 == y0 && a->x1 == x1 && a->y1 == y1) {
            if (period_ms < a->period) a->period = period_ms;
            return;
        }
    }
    GuiAnim *a;
    if (s_anim_n == GUI_ANIM_MAX) {
        /* table full: the last entry grows to cover this box too */
        a = &s_anim[GUI_ANIM_MAX - 1];
        if (x0 > a->x0) x0 = a->x0;
        if (y0 > a->y0) y0 = a->y0;
        if (x1 < a->x1) x1 = a->x1;
        if (y1 < a->y1) y1 = a->y1;
        if (period_ms > a->period) period_ms = a->period;
    } else {
        a = &s_anim[s_anim_n++];
        a->last = s_now_ms;
        a->due = false;
    }
    a->x0 = (int16_t)x0;
    a->y0 = (int16_t)y0;
    a->x1 = (int16_t)x1;
    a->y1 = (int16_t)y1;
    a->period = period_ms;
}

/* A re-render of rows [y0, y1) re-registers the boxes it still draws */
static void gui_anim_forget(int y0, int y1) {
    int k = 0;
    for (int i = 0; i < s_anim_n; i++) {
        if (s_anim[i].y0 >= y0 && s_anim[i].y1 <= y1) continue;
        s_anim[k++] = s_anim[i];
    }
    s_anim_n = k;
    for (int i = 0; i < GUI_EPOCH_MAX; i++) {  /* kept only if drawn again (gui_epoch_sweep) */
        if (s_epoch[i].key && s_epoch[i].y0 >= y0 && s_epoch[i].y1 <= y1) s_epoch[i].stale = true;
    }
}

/* After the render that followed gui_anim_forget: boxes not drawn again are gone */
static void gui_epoch_sweep(void) {
    for (int i = 0; i < GUI_EPOCH_MAX; i++) {
        if (s_epoch[i].stale) s_epoch[i].key = 0;
    }
}

/* The time a marquee box first appeared (now, for a new one) */
static uint32_t gui_epoch_start(const char *str, int x, int y, int w, int h, int dir) {
    uint32_t key = 2166136261u;
    int box[5] = { x, y, w, h, dir };
    for (int i = 0; i < 5; i++) { key ^= (uint32_t)box[i]; key *= 16777619u; }
    for (const unsigned char *p = (const unsigned char *)str; *p; p++) { key ^= *p; key *= 16777619u; }
    if (key == 0) key = 1;
    int slot = 0;
    for (int i = 0; i < GUI_EPOCH_MAX; i++) {
        if (s_epoch[i].key == key) {
            s_epoch[i].stale = false;
            s_epoch[i].seen = s_now_ms;
            return s_epoch[i].start;
        }
        if (!s_epoch[i].key) slot = i;
        else if (s_epoch[slot].key && s_now_ms - s_epoch[i].seen > s_now_ms - s_epoch[slot].seen) slot = i;
    }
    GuiEpoch *e = &s_epoch[slot];
    e->key = key;
    e->start = s_now_ms;
    e->seen = s_now_ms;
    e->y0 = (int16_t)y;
    e->y1 = (int16_t)(y + h);
    e->stale = false;
    return e->start;
}

/* BAND mode with a panel that takes rectangles (or the simulator's VFB):
 * a moving box is redrawn on its own; otherwise its rows are re-rendered */
static bool gui_rect_output_ok(void);

bool mini_gui_animate(uint32_t now_ms) {
    s_now_ms = now_ms;
    bool any = false;
    bool rect = gui_rect_output_ok();
    for (int i = 0; i < s_anim_n; i++) {
        GuiAnim *a = &s_anim[i];
        if (now_ms - a->last < a->period) continue;
        a->last = now_ms;
        if (rect) a->due = true;
        else gui_queue_rows(a->y0, a->y1 - a->y0);
        any = true;
    }
    return any;
}

static const char *const s_marquee_names[5] = { "none", "pixel", "char", "line", "page" };

/* Modes each kind takes: one line has no lines to move by, wrapped text no characters */
static bool marquee_mode_ok(int which, int mode) {
    if (mode < MINI_GUI_MARQUEE_NONE || mode > MINI_GUI_MARQUEE_PAGE) return false;
    if (which == MINI_GUI_TEXT_LINE) return mode != MINI_GUI_MARQUEE_LINE;
    return mode != MINI_GUI_MARQUEE_CHAR;
}

void mini_gui_set_marquee(int which, int mode, int fps_x10, int step) {
    GuiMarquee *m = &s_marquee[which == MINI_GUI_TEXT_WRAP ? 1 : 0];
    if (marquee_mode_ok(which, mode)) m->mode = mode;
    m->fps_x10 = fps_x10 < 1 ? 1 : (fps_x10 > 250 ? 250 : fps_x10);
    m->step = step < 1 ? 1 : (step > 64 ? 64 : step);
    mini_gui_invalidate(0, s_height);
}

void mini_gui_set_marquee_hold(int which, int hold_ms) {
    s_marquee[which == MINI_GUI_TEXT_WRAP ? 1 : 0].hold_ms = hold_ms < 0 ? 0 : (hold_ms > 10000 ? 10000 : hold_ms);
}

int mini_gui_get_marquee_hold(int which) {
    return s_marquee[which == MINI_GUI_TEXT_WRAP ? 1 : 0].hold_ms;
}

void mini_gui_get_marquee(int which, int *mode, int *fps_x10, int *step) {
    const GuiMarquee *m = &s_marquee[which == MINI_GUI_TEXT_WRAP ? 1 : 0];
    if (mode) *mode = m->mode;
    if (fps_x10) *fps_x10 = m->fps_x10;
    if (step) *step = m->step;
}

const char *mini_gui_marquee_name(int mode) {
    return (mode >= 0 && mode < 5) ? s_marquee_names[mode] : "none";
}

int mini_gui_marquee_parse(int which, const char *name) {
    for (int i = 0; i < 5; i++) {
        if (name && strcmp(name, s_marquee_names[i]) == 0) return marquee_mode_ok(which, i) ? i : -1;
    }
    return -1;
}

/* Marquee by characters: the pixel offset c characters in; past the end of
 * the n characters, `blank` more pixels for each */
static int gui_char_pos(const MiniFont *font, const char *str, int n, int c, int extent, int blank) {
    if (c >= n) return extent + (c - n) * blank;
    const unsigned char *p = (const unsigned char *)str;
    int x = 0;
    while (c > 0 && *p) {
        x += gui_char_width(font, utf8_next(&p));
        c--;
    }
    return x;
}

static int gui_char_count(const char *str) {
    int n = 0;
    for (const unsigned char *p = (const unsigned char *)str; *p; p++) n += (*p & 0xC0) != 0x80;
    return n;
}

/* Next code point of a UTF-8 string; 0 for a skipped 4-byte sequence */
static uint16_t utf8_next(const unsigned char **pp) {
    const unsigned char *p = *pp;
    unsigned char c = *p++;
    uint16_t cp = c;
    if ((c & 0xF8) == 0xF0) {
        cp = 0;
        for (int i = 0; i < 3 && *p; i++) p++;
    } else if ((c & 0xF0) == 0xE0 && p[0] && p[1]) {
        cp = (uint16_t)(((c & 0x0F) << 12) | ((p[0] & 0x3F) << 6) | (p[1] & 0x3F));
        p += 2;
    } else if ((c & 0xE0) == 0xC0 && p[0]) {
        cp = (uint16_t)(((c & 0x1F) << 6) | (p[0] & 0x3F));
        p += 1;
    }
    *pp = p;
    return cp;
}

/* Same widths as mini_font_text_width */
static int gui_char_width(const MiniFont *font, uint16_t cp) {
    return cp == 0 ? 0 : mini_font_advance(font, cp);
}

/* Line breaking for mixed Chinese and Latin text: a line may end at a
 * space, or between two characters when either is CJK; Latin words and
 * numbers ("4.5", "U17") stay whole. Closing punctuation never starts a
 * line and opening punctuation never ends one (kinsoku). */
static bool gui_is_cjk(uint16_t cp) {
    return (cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFF00 && cp <= 0xFFEF);
}

static bool gui_no_line_start(uint16_t cp) {
    static const uint16_t list[] = {
        0xFF0C, 0x3002, 0x3001, 0xFF1B, 0xFF1A, 0xFF1F, 0xFF01, 0xFF09, 0x300D, 0x300F, 0x3011,
        0x300B, 0x3009, 0x201D, 0x2019, 0x2026, 0x00B7, 0xFF0E, 0xFF05, 0xFF5D, 0xFF3D,
        ',', '.', ';', ':', '?', '!', ')', ']', '}', '%',
    };
    for (size_t i = 0; i < sizeof(list) / sizeof(list[0]); i++) {
        if (list[i] == cp) return true;
    }
    return false;
}

static bool gui_no_line_end(uint16_t cp) {
    static const uint16_t list[] = {
        0xFF08, 0x300C, 0x300E, 0x3010, 0x300A, 0x3008, 0x201C, 0x2018, 0xFF5B, 0xFF3B,
        '(', '[', '{',
    };
    for (size_t i = 0; i < sizeof(list) / sizeof(list[0]); i++) {
        if (list[i] == cp) return true;
    }
    return false;
}

/* End of the line starting at s that fits width w, at the last place a
 * line may end (see above); with none, between any two characters. '\n'
 * forces a break. *next receives where the following line starts (after
 * the space a line ends at). */
static const char *gui_wrap_line(const char *s, const MiniFont *font, int w, const char **next) {
    const unsigned char *p = (const unsigned char *)s;
    const unsigned char *brk_end = NULL, *brk_next = NULL;  /* the last allowed break */
    uint16_t prev = 0;
    int width = 0;
    while (*p && *p != '\n') {
        const unsigned char *q = p;
        uint16_t cp = utf8_next(&q);
        int cw = gui_char_width(font, cp);
        if (p != (const unsigned char *)s) {
            if (prev == ' ' && cp != ' ') {
                brk_end = p - 1;  /* the line ends at the space */
                brk_next = p;
            } else if (cp != ' ' && prev != ' ' && (gui_is_cjk(prev) || gui_is_cjk(cp)) &&
                       !gui_no_line_start(cp) && !gui_no_line_end(prev)) {
                brk_end = p;
                brk_next = p;
            }
        }
        if (width + cw > w && p != (const unsigned char *)s) {
            if (cp == ' ') {         /* overflowing on a space: the line ends there */
                *next = (const char *)q;
                return (const char *)p;
            }
            if (brk_end) {
                *next = (const char *)brk_next;
                return (const char *)brk_end;
            }
            *next = (const char *)p;
            return (const char *)p;
        }
        width += cw;
        prev = cp;
        p = q;
    }
    *next = (const char *)(*p == '\n' ? p + 1 : p);
    return (const char *)p;
}

int mini_gui_wrap_text(const char *str, int font_id, int w, int *starts, int *ends, int max) {
    const MiniFont *font = mini_font_get(font_id);
    if (!str || !font || w <= 0) return 0;
    int n = 0;
    for (const char *p = str, *next; *p; p = next) {
        const char *end = gui_wrap_line(p, font, w, &next);
        if (n < max) {
            if (starts) starts[n] = (int)(p - str);
            if (ends) ends[n] = (int)(end - str);
        }
        n++;
        if (next == p) break;
    }
    return n;
}

/* Wrapped-line table of a vertical scroller: every band of every frame
 * needs it, so it is computed once per text, font and width. The tables are
 * word memory (IRAM on the ESP32, allocated on first use): every field is a
 * whole 32-bit word, a line's start and end packed in one, read and written
 * through volatile so the compiler never loads a halfword. */
#define GUI_WRAP_SLOTS 4
#define GUI_WRAP_LINES 128
typedef struct {
    uint32_t key;      /* 0 = empty */
    uint32_t n;
    uint32_t line[GUI_WRAP_LINES];   /* start | end << 16 (byte offsets in the text) */
} GuiWrap;
static GuiWrap *s_wrap = NULL;   /* [GUI_WRAP_SLOTS] */

static uint32_t wrap_get(const volatile uint32_t *w) { return *w; }
static void wrap_set(volatile uint32_t *w, uint32_t v) { *w = v; }
#define WRAP_START(t, i) ((int)(wrap_get(&(t)->line[i]) & 0xFFFF))
#define WRAP_END(t, i)   ((int)(wrap_get(&(t)->line[i]) >> 16))
#define WRAP_N(t)        ((int)wrap_get(&(t)->n))
static int s_wrap_next = 0;

static const GuiWrap *gui_wrap_table(const char *str, const MiniFont *font, int font_id, int w) {
    uint32_t key = 2166136261u ^ (uint32_t)font_id ^ ((uint32_t)w << 16);
    for (const unsigned char *p = (const unsigned char *)str; *p; p++) { key ^= *p; key *= 16777619u; }
    if (key == 0) key = 1;
    if (!s_wrap) {
        s_wrap = (GuiWrap *)mini_word_alloc(GUI_WRAP_SLOTS * sizeof(GuiWrap));
        if (!s_wrap) return NULL;
        mini_words_fill(s_wrap, 0, GUI_WRAP_SLOTS * sizeof(GuiWrap));
    }
    for (int i = 0; i < GUI_WRAP_SLOTS; i++) {
        if (wrap_get(&s_wrap[i].key) == key) return &s_wrap[i];
    }
    if (strlen(str) > 0xFFFF) return NULL;
    GuiWrap *t = &s_wrap[s_wrap_next];
    s_wrap_next = (s_wrap_next + 1) % GUI_WRAP_SLOTS;
    wrap_set(&t->key, key);
    uint32_t n = 0;
    for (const char *p = str, *next; *p && n < GUI_WRAP_LINES; p = next) {
        const char *end = gui_wrap_line(p, font, w, &next);
        wrap_set(&t->line[n], (uint32_t)(p - str) | (uint32_t)(end - str) << 16);
        n++;
        if (next == p) break;
    }
    wrap_set(&t->n, n);
    return t;
}

/* Draws [a, b) of a string at x y */
static void gui_text_span(int x, int y, const char *a, const char *b, uint16_t color, int font_id) {
    char line[256];
    size_t n = (size_t)(b - a);
    if (n >= sizeof(line)) n = sizeof(line) - 1;
    memcpy(line, a, n);
    line[n] = '\0';
    mini_gui_text_styled(x, y, line, color, font_id);
}

int mini_gui_text_wrap(int x, int y, int w, int h, const char *str, uint16_t color, int font_id, int line_h) {
    const MiniFont *font = mini_font_get(font_id);
    if (!str || !font || w <= 0 || h <= 0) return 0;
    if (line_h <= 0) line_h = font->height + 2;
    int max_lines = h / line_h;
    if (max_lines < 1) max_lines = 1;
    int lines = 0;
    const char *p = str, *next;
    while (*p && lines < max_lines) {
        const char *end = gui_wrap_line(p, font, w, &next);
        if (lines == max_lines - 1 && *next) {
            /* the last line that fits, with more text behind: end it with an ellipsis */
            const MiniFont *used;
            const char *ell = mini_font_glyph(font, 0x2026, &used) ? "\xe2\x80\xa6" : "...";
            size_t el = strlen(ell) + 1;
            int ew = mini_font_text_width(font_id, ell);
            gui_wrap_line(p, font, w - ew, &next);
            end = next;
            while (end > p && end[-1] == ' ') end--;
            char line[256];
            size_t n = (size_t)(end - p);
            if (n > sizeof(line) - el) n = sizeof(line) - el;
            memcpy(line, p, n);
            memcpy(line + n, ell, el);
            mini_gui_text_styled(x, y + lines * line_h, line, color, font_id);
            return lines + 1;
        }
        gui_text_span(x, y + lines * line_h, p, end, color, font_id);
        lines++;
        if (next == p) break;
        p = next;
    }
    return lines;
}

void mini_gui_text_scroll(int x, int y, int w, int h, const char *str, uint16_t color,
                          int font_id, int speed, int dir) {
    if (!str || w <= 0 || h <= 0) return;
    if (s_rec_on) {
        gui_rec(GUI_OP_TEXT_SCROLL, x, y, w, h, font_id, (uint8_t)dir, color, (uint16_t)speed, 0, 0, str);
    } else if (GUI_ROWS_HIDDEN(y, h)) {
        return; /* other bands: the recording pass already registered the box */
    }
    const MiniFont *font = mini_font_get(font_id);
    if (!font) return;
    bool fit = dir == MINI_GUI_SCROLL_FIT, area = dir == MINI_GUI_SCROLL_AREA;
    const GuiMarquee *mq = fit ? &s_marquee[0] : (area ? &s_marquee[1] : NULL);
    int line_h = (area && speed > 0) ? speed : font->height + 2; /* AREA: speed carries the line height */
    if (mq && mq->mode == MINI_GUI_MARQUEE_NONE) {
        if (!s_rec_on) mini_gui_text_wrap(x, y, w, h, str, color, font_id, area ? line_h : h);
        return;
    }

    /* Measure: one line across, or wrapped lines down (one-line pages are the
     * pieces that fit the width, so they come from the wrap table too) */
    bool vertical = dir == MINI_GUI_SCROLL_UP || area;
    bool pieces = fit && mq->mode == MINI_GUI_MARQUEE_PAGE;
    int extent, box;
    const GuiWrap *wrap = NULL;
    if (vertical || pieces) {
        wrap = gui_wrap_table(str, font, font_id, w);
        if (!wrap) return;
    }
    if (vertical) {
        extent = WRAP_N(wrap) * line_h;
        box = h;
    } else {
        extent = mini_font_text_width(font_id, str);
        box = w;
    }
    bool moving = extent > box && (speed > 0 || mq);

    /* The box on screen, nested in the caller's clip */
    int old_x0 = s_uclip_x0, old_y0 = s_uclip_y0, old_x1 = s_uclip_x1, old_y1 = s_uclip_y1;
    int bx0 = x > old_x0 ? x : old_x0, by0 = y > old_y0 ? y : old_y0;
    int bx1 = (x + w) < old_x1 ? (x + w) : old_x1, by1 = (y + h) < old_y1 ? (y + h) : old_y1;
    uint32_t born = (moving && mq) ? gui_epoch_start(str, x, y, w, h, dir) : 0;
    if (moving && !s_active_sub_buffer) {
        int ax0 = bx0 < 0 ? 0 : bx0, ay0 = by0 < 0 ? 0 : by0;
        int ax1 = bx1 > s_width ? s_width : bx1, ay1 = by1 > s_height ? s_height : by1;
        uint32_t every = mq ? (uint32_t)(10000 / mq->fps_x10) : GUI_ANIM_FRAME_MS;
        if (ax0 < ax1 && ay0 < ay1) gui_anim_add(ax0, ay0, ax1, ay1, every);
    }
    if (s_rec_on || GUI_ROWS_HIDDEN(y, h)) return;

    /* Offset from the frame clock: `steps` refreshes so far. gui-text-scroll
     * loops with a gap. The marquees are paced for the eye: every step has
     * the same length (no shortened last step), one line runs until its end
     * is in view with at least one blank character behind it, wrapped text
     * until its last line is in view; then the first character (line) shows
     * again and stays hold_ms (config.ini [text] marquee_hold, 1 s), for the
     * eye to find the start, when the steps are faster than that. The clock
     * is the box's own (gui_epoch_start): a box that has just appeared
     * starts with the hold too, not in the middle of a cycle. */
    int32_t age = (int32_t)(s_frame_ms - born);  /* ms since the box appeared */
    uint64_t steps = (mq && age > 0) ? (uint64_t)age * (uint64_t)mq->fps_x10 / 10000u : 0;
    int mode = mq ? mq->mode : MINI_GUI_MARQUEE_PIXEL;
    int gap = box / 3 > 24 ? box / 3 : 24;
    int period = extent + gap;
    int off = 0, page_first = 0, page_lines = wrap ? WRAP_N(wrap) : 0;
    if (moving && !mq) {
        off = (int)(((uint64_t)s_frame_ms * (uint64_t)speed / 1000u) % (uint64_t)period);
    } else if (moving && mode != MINI_GUI_MARQUEE_PAGE) {
        int n = vertical ? WRAP_N(wrap) : gui_char_count(str);
        int avg = n > 0 ? extent / n : font->height;  /* the average character */
        int limit = extent - box + (vertical ? 0 : avg); /* reached: the end in view (+ one blank) */
        int interval = 10000 / mq->fps_x10;
        int dwell = mq->hold_ms / interval;            /* extra refreshes on the start */
        int last;                                      /* the step that reaches the limit */
        if (mode == MINI_GUI_MARQUEE_CHAR) {
            /* positions on character boundaries, step characters apart */
            last = 0;
            while (gui_char_pos(font, str, n, last * mq->step, extent, avg) < limit) last++;
        } else {
            int unit = mode == MINI_GUI_MARQUEE_LINE ? mq->step * line_h : mq->step;
            last = (limit + unit - 1) / unit;
        }
        int k = (int)(steps % (uint64_t)(last + 1 + dwell));
        int at = k <= dwell ? 0 : k - dwell;
        if (mode == MINI_GUI_MARQUEE_CHAR) off = gui_char_pos(font, str, n, at * mq->step, extent, avg);
        else off = at * (mode == MINI_GUI_MARQUEE_LINE ? mq->step * line_h : mq->step);
    } else if (moving && mode == MINI_GUI_MARQUEE_PAGE) {
        /* the box shows one piece at a time: a box-full of lines, or the
         * part of the line that fits the width */
        int per = vertical ? (h / line_h > 0 ? h / line_h : 1) : 1;
        int pages = (WRAP_N(wrap) + per - 1) / per;
        page_first = (int)(steps % (uint64_t)pages) * per;
        page_lines = per;
    }

    mini_gui_set_clip(bx0, by0, bx1 - bx0, by1 - by0);
    if (vertical) {
        bool paged = moving && mq && mode == MINI_GUI_MARQUEE_PAGE;
        int copies = (moving && !mq) ? 2 : 1;  /* gui-text-scroll loops; marquees start over */
        for (int copy = 0; copy < copies; copy++) {
            int top = paged ? y - page_first * line_h : y - off + copy * period;
            int last = paged ? page_first + page_lines : WRAP_N(wrap);
            if (last > WRAP_N(wrap)) last = WRAP_N(wrap);
            /* only the lines crossing the clip (the current band, inside the box) */
            int first = (s_clip_y0 - top - line_h) / line_h;
            if (first < (paged ? page_first : 0)) first = paged ? page_first : 0;
            for (int i = first; i < last; i++) {
                int ly = top + i * line_h;
                if (ly >= s_clip_y1) break;
                if (ly + line_h > s_clip_y0) gui_text_span(x, ly, str + WRAP_START(wrap, i), str + WRAP_END(wrap, i), color, font_id);
            }
        }
    } else if (moving && pieces) {
        int ty = y;
        gui_text_span(x, ty, str + WRAP_START(wrap, page_first), str + WRAP_END(wrap, page_first), color, font_id);
    } else {
        int ty = fit ? y : y + (h - font->height) / 2;
        mini_gui_text_styled(x - off, ty, str, color, font_id);
        if (moving && !mq && x - off + period < x + w) mini_gui_text_styled(x - off + period, ty, str, color, font_id);
    }
    s_uclip_x0 = old_x0; s_uclip_y0 = old_y0; s_uclip_x1 = old_x1; s_uclip_y1 = old_y1;
    gui_apply_clip();
}

/* ========================================================================= */
/* Seven-Segment VFD Renderer                                                */
/* ========================================================================= */

/* Segment bits: a=0x01 b=0x02 c=0x04 d=0x08 e=0x10 f=0x20 g=0x40 */
static uint8_t seg7_mask(char ch) {
    static const uint8_t digits[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };
    if (ch >= '0' && ch <= '9') return digits[ch - '0'];
    if (ch == '-') return 0x40;
    return 0;
}

/* Horizontal hexagonal bar centred on row cy, thickness t, spanning [x0, x1) */
static void seg7_hbar(int x0, int x1, int cy, int t, uint16_t color) {
    int half = t / 2;
    for (int r = -half; r < t - half; r++) {
        int inset = r < 0 ? -r : r;
        if (x1 - inset > x0 + inset) mini_gui_rect(x0 + inset, cy + r, (x1 - inset) - (x0 + inset), 1, color, true);
    }
}

/* Vertical hexagonal bar centred on column cx, thickness t, spanning [y0, y1) */
static void seg7_vbar(int y0, int y1, int cx, int t, uint16_t color) {
    int half = t / 2;
    for (int c = -half; c < t - half; c++) {
        int inset = c < 0 ? -c : c;
        if (y1 - inset > y0 + inset) mini_gui_rect(cx + c, y0 + inset, 1, (y1 - inset) - (y0 + inset), color, true);
    }
}

/* One segment (0..6 = a..g). Centre lines come from layout thickness t;
 * the bar is drawn with thickness dt (> t for glow, 1 for the highlight). */
static void seg7_segment(int seg, int x, int y, int w, int h, int t, int dt, uint16_t color) {
    int half = t / 2;
    int gap = 1 + t / 6 - (dt - t) / 2;   /* cut between segments, closed by glow */
    int left = x + half, right = x + w - 1 - half;
    int top = y + half, mid = y + h / 2, bottom = y + h - 1 - half;
    switch (seg) {
        case 0: seg7_hbar(left + gap, right - gap + 1, top, dt, color); break;
        case 1: seg7_vbar(top + gap, mid - gap + 1, right, dt, color); break;
        case 2: seg7_vbar(mid + gap, bottom - gap + 1, right, dt, color); break;
        case 3: seg7_hbar(left + gap, right - gap + 1, bottom, dt, color); break;
        case 4: seg7_vbar(mid + gap, bottom - gap + 1, left, dt, color); break;
        case 5: seg7_vbar(top + gap, mid - gap + 1, left, dt, color); break;
        default: seg7_hbar(left + gap, right - gap + 1, mid, dt, color); break;
    }
}

/* Halfway blend of two RGB565 colours (used for the outer bloom) */
static uint16_t rgb565_mix(uint16_t a, uint16_t b) {
    return (uint16_t)((((a & 0xF7DE) >> 1) + ((b & 0xF7DE) >> 1)));
}

/* Draws str ("12:34", "--:--") as seven-segment digits; returns the width.
 * Unlit segments use off_col (ghost). Lit segments get a glow halo unless
 * glow_col == on_col and a centre highlight unless hi_col == on_col, so
 * passing on_col for both gives flat LCD-style segments. */
int mini_gui_seg7(int x, int y, int w, int h, int t, const char *str,
                  uint16_t on_col, uint16_t off_col, uint16_t glow_col, uint16_t hi_col) {
    if (!str || w <= 0 || h <= 0 || t <= 0) return 0;
    if (s_rec_on) {
        gui_rec(GUI_OP_SEG7, x, y, w, h, t, 0, on_col, off_col, glow_col, hi_col, str);
        return mini_gui_seg7_width(w, t, str);
    }
    int spacing = t / 2 + 3;
    int cx = x;
    /* Skip whole glyph rows outside the active clip (band mode) */
    bool visible = (y - 2) < s_clip_y1 && (y + h + 2) > s_clip_y0;
    for (const char *p = str; *p; p++) {
        if (*p == ':' || *p == '.') {
            int dot = t;
            if (visible) {
                int dx = cx + 1;
                int ys[2] = { y + h / 3 - dot / 2, y + (2 * h) / 3 - dot / 2 };
                for (int i = 0; i < (*p == ':' ? 2 : 1); i++) {
                    int dy = (*p == ':') ? ys[i] : y + h - dot;
                    if (glow_col != on_col) {
                        mini_gui_rect(dx - 2, dy - 2, dot + 4, dot + 4, rgb565_mix(glow_col, off_col), true);
                        mini_gui_rect(dx - 1, dy - 1, dot + 2, dot + 2, glow_col, true);
                    }
                    mini_gui_rect(dx, dy, dot, dot, on_col, true);
                }
            }
            cx += dot + 2 + spacing;
            continue;
        }
        if (visible) {
            uint8_t mask = seg7_mask(*p);
            bool glow = glow_col != on_col;
            uint16_t bloom = rgb565_mix(glow_col, off_col);
            for (int s = 0; s < 7; s++) {
                if (!(mask & (1u << s))) seg7_segment(s, cx, y, w, h, t, t, off_col);
            }
            /* Lit segments: optional bloom + glow halo, core, optional highlight */
            for (int s = 0; glow && s < 7; s++) {
                if (mask & (1u << s)) seg7_segment(s, cx, y, w, h, t, t + 4, bloom);
            }
            for (int s = 0; glow && s < 7; s++) {
                if (mask & (1u << s)) seg7_segment(s, cx, y, w, h, t, t + 2, glow_col);
            }
            for (int s = 0; s < 7; s++) {
                if (mask & (1u << s)) seg7_segment(s, cx, y, w, h, t, t, on_col);
            }
            if (t >= 5 && hi_col != on_col) {
                for (int s = 0; s < 7; s++) {
                    if (mask & (1u << s)) seg7_segment(s, cx, y, w, h, t, t >= 8 ? 2 : 1, hi_col);
                }
            }
        }
        cx += w + spacing;
    }
    return cx - x - spacing;
}

/* Width that mini_gui_seg7 would use for str */
int mini_gui_seg7_width(int w, int t, const char *str) {
    if (!str) return 0;
    int spacing = t / 2 + 3, total = 0, n = 0;
    for (const char *p = str; *p; p++, n++) total += (*p == ':' || *p == '.') ? t + 2 : w;
    return n ? total + spacing * (n - 1) : 0;
}

void mini_gui_inject_touch(bool is_down, int x, int y) {
    s_touch_down = is_down;
    s_touch_x = x;
    s_touch_y = y;
}

bool mini_gui_get_touch(int *out_x, int *out_y) {
    if (out_x) *out_x = s_touch_x;
    if (out_y) *out_y = s_touch_y;
    return s_touch_down;
}

bool mini_gui_is_touch_down(void) {
    return s_touch_down;
}

/* ========================================================================= */
/* Scheme Primitive Wrappers                                                 */
/* ========================================================================= */

static Value prim_gui_init(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    int w = MINI_GUI_DEFAULT_WIDTH;
    int h = MINI_GUI_DEFAULT_HEIGHT;
    if (argc >= 2 && mini_is_fixnum(argv[0]) && mini_is_fixnum(argv[1])) {
        w = (int)mini_to_fixnum(argv[0]);
        h = (int)mini_to_fixnum(argv[1]);
    }
    mini_gui_init(w, h);
    return mini_void();
}

static Value prim_gui_clear(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    uint16_t color = MINI_COLOR_BG_DARK;
    if (argc >= 1 && mini_is_fixnum(argv[0])) {
        color = (uint16_t)mini_to_fixnum(argv[0]);
    }
    mini_gui_clear(color);
    return mini_void();
}

static Value prim_gui_flush(MiniVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc; (void)argv;
    mini_gui_flush();
    return mini_void();
}

static Value prim_gui_pixel(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    if (argc >= 3 && mini_is_fixnum(argv[0]) && mini_is_fixnum(argv[1]) && mini_is_fixnum(argv[2])) {
        mini_gui_pixel((int)mini_to_fixnum(argv[0]),
                       (int)mini_to_fixnum(argv[1]),
                       (uint16_t)mini_to_fixnum(argv[2]));
        return mini_void();
    }
    if (argc == 2 && mini_is_fixnum(argv[0]) && mini_is_fixnum(argv[1])) {
        uint16_t c = mini_gui_get_pixel((int)mini_to_fixnum(argv[0]), (int)mini_to_fixnum(argv[1]));
        return mini_make_fixnum((int64_t)c);
    }
    return mini_void();
}

static Value prim_gui_line(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    if (argc >= 5 && mini_is_fixnum(argv[0]) && mini_is_fixnum(argv[1]) &&
        mini_is_fixnum(argv[2]) && mini_is_fixnum(argv[3]) && mini_is_fixnum(argv[4])) {
        mini_gui_line((int)mini_to_fixnum(argv[0]),
                      (int)mini_to_fixnum(argv[1]),
                      (int)mini_to_fixnum(argv[2]),
                      (int)mini_to_fixnum(argv[3]),
                      (uint16_t)mini_to_fixnum(argv[4]));
    }
    return mini_void();
}

static Value prim_gui_rect(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    if (argc >= 5 && mini_is_fixnum(argv[0]) && mini_is_fixnum(argv[1]) &&
        mini_is_fixnum(argv[2]) && mini_is_fixnum(argv[3]) && mini_is_fixnum(argv[4])) {
        bool filled = (argc >= 6) ? mini_is_true(argv[5]) : true;
        mini_gui_rect((int)mini_to_fixnum(argv[0]),
                      (int)mini_to_fixnum(argv[1]),
                      (int)mini_to_fixnum(argv[2]),
                      (int)mini_to_fixnum(argv[3]),
                      (uint16_t)mini_to_fixnum(argv[4]),
                      filled);
    }
    return mini_void();
}

static Value prim_gui_round_rect(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    if (argc >= 6 && mini_is_fixnum(argv[0]) && mini_is_fixnum(argv[1]) &&
        mini_is_fixnum(argv[2]) && mini_is_fixnum(argv[3]) && mini_is_fixnum(argv[4]) &&
        mini_is_fixnum(argv[5])) {
        bool filled = (argc >= 7) ? mini_is_true(argv[6]) : true;
        mini_gui_round_rect((int)mini_to_fixnum(argv[0]),
                            (int)mini_to_fixnum(argv[1]),
                            (int)mini_to_fixnum(argv[2]),
                            (int)mini_to_fixnum(argv[3]),
                            (int)mini_to_fixnum(argv[4]),
                            (uint16_t)mini_to_fixnum(argv[5]),
                            filled);
    }
    return mini_void();
}

static Value prim_gui_circle(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    if (argc >= 4 && mini_is_fixnum(argv[0]) && mini_is_fixnum(argv[1]) &&
        mini_is_fixnum(argv[2]) && mini_is_fixnum(argv[3])) {
        bool filled = (argc >= 5) ? mini_is_true(argv[4]) : true;
        mini_gui_circle((int)mini_to_fixnum(argv[0]),
                        (int)mini_to_fixnum(argv[1]),
                        (int)mini_to_fixnum(argv[2]),
                        (uint16_t)mini_to_fixnum(argv[3]),
                        filled);
    }
    return mini_void();
}

static Value prim_gui_text(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    if (argc >= 4 && mini_is_fixnum(argv[0]) && mini_is_fixnum(argv[1]) &&
        mini_is_string(argv[2]) && mini_is_fixnum(argv[3])) {
        int size = (argc >= 5 && mini_is_fixnum(argv[4])) ? (int)mini_to_fixnum(argv[4]) : 1;
        if (size == 1) {
            mini_gui_text_styled((int)mini_to_fixnum(argv[0]),
                                 (int)mini_to_fixnum(argv[1]),
                                 mini_string_data(argv[2]),
                                 (uint16_t)mini_to_fixnum(argv[3]),
                                 s_current_font);
        } else {
            mini_gui_text((int)mini_to_fixnum(argv[0]),
                          (int)mini_to_fixnum(argv[1]),
                          mini_string_data(argv[2]),
                          (uint16_t)mini_to_fixnum(argv[3]),
                          size);
        }
    }
    return mini_void();
}

static Value prim_gui_set_font(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    if (argc >= 1 && mini_is_fixnum(argv[0])) {
        mini_gui_set_font((int)mini_to_fixnum(argv[0]));
    }
    return mini_void();
}

static Value prim_gui_get_font(MiniVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc; (void)argv;
    return mini_make_fixnum(mini_gui_get_font());
}

static Value prim_gui_text_width(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    if (argc >= 1 && mini_is_string(argv[0])) {
        int fid = (argc >= 2 && mini_is_fixnum(argv[1])) ? (int)mini_to_fixnum(argv[1]) : s_current_font;
        return mini_make_fixnum(mini_gui_text_width(mini_string_data(argv[0]), fid));
    }
    return mini_make_fixnum(0);
}

static Value prim_gui_text_styled(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    if (argc >= 4 && mini_is_fixnum(argv[0]) && mini_is_fixnum(argv[1]) &&
        mini_is_string(argv[2]) && mini_is_fixnum(argv[3])) {
        int fid = (argc >= 5 && mini_is_fixnum(argv[4])) ? (int)mini_to_fixnum(argv[4]) : s_current_font;
        mini_gui_text_styled((int)mini_to_fixnum(argv[0]),
                             (int)mini_to_fixnum(argv[1]),
                             mini_string_data(argv[2]),
                             (uint16_t)mini_to_fixnum(argv[3]),
                             fid);
    }
    return mini_void();
}

static Value prim_gui_text_box(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    if (argc >= 6 && mini_is_fixnum(argv[0]) && mini_is_fixnum(argv[1]) &&
        mini_is_fixnum(argv[2]) && mini_is_fixnum(argv[3]) &&
        mini_is_string(argv[4]) && mini_is_fixnum(argv[5])) {
        int fid = (argc >= 7 && mini_is_fixnum(argv[6])) ? (int)mini_to_fixnum(argv[6]) : s_current_font;
        int halign = (argc >= 8 && mini_is_fixnum(argv[7])) ? (int)mini_to_fixnum(argv[7]) : 0;
        int valign = (argc >= 9 && mini_is_fixnum(argv[8])) ? (int)mini_to_fixnum(argv[8]) : 0;
        mini_gui_text_box((int)mini_to_fixnum(argv[0]),
                          (int)mini_to_fixnum(argv[1]),
                          (int)mini_to_fixnum(argv[2]),
                          (int)mini_to_fixnum(argv[3]),
                          mini_string_data(argv[4]),
                          (uint16_t)mini_to_fixnum(argv[5]),
                          fid, halign, valign);
    }
    return mini_void();
}

static Value prim_gui_rgb(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    if (argc >= 3 && mini_is_fixnum(argv[0]) && mini_is_fixnum(argv[1]) && mini_is_fixnum(argv[2])) {
        uint8_t r = (uint8_t)mini_to_fixnum(argv[0]);
        uint8_t g = (uint8_t)mini_to_fixnum(argv[1]);
        uint8_t b = (uint8_t)mini_to_fixnum(argv[2]);
        return mini_make_fixnum(mini_gui_rgb(r, g, b));
    }
    return mini_make_fixnum(0);
}

static Value prim_gui_set_resolution(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    if (argc >= 2 && mini_is_fixnum(argv[0]) && mini_is_fixnum(argv[1])) {
        int w = (int)mini_to_fixnum(argv[0]);
        int h = (int)mini_to_fixnum(argv[1]);
        mini_gui_set_resolution(w, h);
    }
    return mini_void();
}

static Value prim_gui_width(MiniVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc; (void)argv;
    return mini_make_fixnum(mini_gui_get_width());
}

static Value prim_gui_height(MiniVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc; (void)argv;
    return mini_make_fixnum(mini_gui_get_height());
}

static Value prim_gui_touch_poll(MiniVM *vm, int argc, Value *argv) {
    (void)argc; (void)argv;
    int tx = 0, ty = 0;
    if (mini_gui_get_touch(&tx, &ty)) {
        Value vx = mini_make_fixnum(tx);
        Value vy = mini_make_fixnum(ty);
        return mini_make_cons(vm, vx, vy);
    }
    return mini_false();
}

static Value prim_gui_touch_down_p(MiniVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc; (void)argv;
    return mini_make_bool(mini_gui_is_touch_down());
}

static Value prim_ws2812_set(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    if (argc >= 3 && mini_is_fixnum(argv[0]) && mini_is_fixnum(argv[1]) && mini_is_fixnum(argv[2])) {
        uint8_t r = (uint8_t)mini_to_fixnum(argv[0]);
        uint8_t g = (uint8_t)mini_to_fixnum(argv[1]);
        uint8_t b = (uint8_t)mini_to_fixnum(argv[2]);
        mini_ws2812_set(r, g, b);
    }
    return mini_void();
}

static Value prim_ws2812_get(MiniVM *vm, int argc, Value *argv) {
    (void)argc; (void)argv;
    uint8_t r, g, b;
    mini_ws2812_get(&r, &g, &b);
    Value vb = mini_make_cons(vm, mini_make_fixnum(b), mini_nil());
    Value vg = mini_make_cons(vm, mini_make_fixnum(g), vb);
    return mini_make_cons(vm, mini_make_fixnum(r), vg);
}

static Value prim_ws2812_clear(MiniVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc; (void)argv;
    mini_ws2812_set(0, 0, 0);
    return mini_void();
}

static Value prim_light_read(MiniVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc; (void)argv;
    return mini_make_fixnum(mini_light_read());
}

static Value prim_light_lux(MiniVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc; (void)argv;
    int val = mini_light_read();
    int lux = (val * 1000) / 4095;
    return mini_make_fixnum(lux);
}

/* (backlight-set n): a fixed brightness 0..255; the automatic one stops */
static Value prim_backlight_set(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    if (argc >= 1 && mini_is_fixnum(argv[0])) {
        int v = (int)mini_to_fixnum(argv[0]);
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        s_backlight_auto = false;
        mini_backlight_set((uint8_t)v);
        if (s_backlight_hook) s_backlight_hook(false, v);
    }
    return mini_void();
}

/* (backlight-auto) => #t while the light sensor sets the brightness;
 * (backlight-auto #t / #f) turns that on or off */
static Value prim_backlight_auto(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    if (argc >= 1) {
        s_backlight_auto = !mini_is_false(argv[0]);
        if (s_backlight_auto) mini_backlight_auto_step();
        if (s_backlight_hook) s_backlight_hook(s_backlight_auto, s_backlight_val);
    }
    return mini_make_bool(s_backlight_auto);
}

static Value prim_backlight_get(MiniVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc; (void)argv;
    return mini_make_fixnum(mini_backlight_get());
}

/* (backlight-offset) => current offset; (backlight-offset n) sets -10..10 */
static Value prim_backlight_offset(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    if (argc >= 1 && mini_is_fixnum(argv[0])) {
        int v = (int)mini_to_fixnum(argv[0]);
        mini_backlight_set_params(v, s_backlight_min, s_backlight_max);
        if (s_backlight_auto) mini_backlight_auto_step();
    }
    return mini_make_fixnum(s_backlight_offset);
}

/* (backlight-min) => current min; (backlight-min n) sets 0..255 */
static Value prim_backlight_min(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    if (argc >= 1 && mini_is_fixnum(argv[0])) {
        int v = (int)mini_to_fixnum(argv[0]);
        mini_backlight_set_params(s_backlight_offset, v, s_backlight_max);
        if (s_backlight_auto) mini_backlight_auto_step();
    }
    return mini_make_fixnum(s_backlight_min);
}

/* (backlight-max) => current max; (backlight-max n) sets 0..255 */
static Value prim_backlight_max(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    if (argc >= 1 && mini_is_fixnum(argv[0])) {
        int v = (int)mini_to_fixnum(argv[0]);
        mini_backlight_set_params(s_backlight_offset, s_backlight_min, v);
        if (s_backlight_auto) mini_backlight_auto_step();
    }
    return mini_make_fixnum(s_backlight_max);
}

static Value prim_button_read(MiniVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc; (void)argv;
    return mini_make_fixnum(mini_button_read() ? 1 : 0);
}

static Value prim_button_pressed_p(MiniVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc; (void)argv;
    return mini_make_bool(mini_button_read());
}

static Value prim_display_model(MiniVM *vm, int argc, Value *argv) {
    (void)argc; (void)argv;
    return mini_make_string(vm, "ILI9488-8BIT-PARALLEL-480x320");
}

static Value prim_display_rotation(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
#if defined(ESP_PLATFORM)
    if (argc >= 1 && mini_is_fixnum(argv[0])) {
        uint8_t r = (uint8_t)(mini_to_fixnum(argv[0]) % 4);
        esp32_ili9488_set_rotation(r);
        int w = esp32_ili9488_get_width();
        int h = esp32_ili9488_get_height();
        mini_gui_init(w, h);
        mini_gui_mark_dirty_rect(0, 0, w, h);
        return mini_make_fixnum(r);
    }
    return mini_make_fixnum(esp32_ili9488_get_rotation());
#else
    static int s_sim_rot = 0;
    if (argc >= 1 && mini_is_fixnum(argv[0])) {
        s_sim_rot = (int)(mini_to_fixnum(argv[0]) % 4);
        int w = (s_sim_rot % 2 == 0) ? 320 : 480;
        int h = (s_sim_rot % 2 == 0) ? 480 : 320;
        mini_gui_init(w, h);
        mini_gui_mark_dirty_rect(0, 0, w, h);
        return mini_make_fixnum(s_sim_rot);
    }
    return mini_make_fixnum(s_sim_rot);
#endif
}


static Value prim_gui_set_clip(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    if (argc >= 4 && mini_is_fixnum(argv[0]) && mini_is_fixnum(argv[1]) &&
        mini_is_fixnum(argv[2]) && mini_is_fixnum(argv[3])) {
        mini_gui_set_clip((int)mini_to_fixnum(argv[0]),
                          (int)mini_to_fixnum(argv[1]),
                          (int)mini_to_fixnum(argv[2]),
                          (int)mini_to_fixnum(argv[3]));
    }
    return mini_void();
}

/* (gui-chart x y w h ring start count lo hi color): polyline of count points
 * taken from the ring vector beginning at index start; values are clamped
 * to [lo, hi] and scaled to the box (one display-list command) */
/* (gui-bitmap x y w h data scale fg [bg]) */
static Value prim_gui_bitmap(MiniVM *vm, int argc, Value *argv) {
    if (argc < 7 || !mini_is_string(argv[4])) {
        vm_error(vm, "gui-bitmap: expected x y w h data scale fg [bg]");
        return mini_void();
    }
    for (int i = 0; i < 7; i++) {
        if (i != 4 && !mini_is_fixnum(argv[i])) {
            vm_error(vm, "gui-bitmap: argument %d must be an integer", i + 1);
            return mini_void();
        }
    }
    int64_t w = mini_to_fixnum(argv[2]), h = mini_to_fixnum(argv[3]), scale = mini_to_fixnum(argv[5]);
    if (w <= 0 || h <= 0 || scale <= 0 || w > 4096 || h > 4096 || scale > 255) {
        vm_error(vm, "gui-bitmap: w and h must be 1..4096, scale 1..255");
        return mini_void();
    }
    size_t need = (size_t)h * (size_t)((w + 7) / 8);
    if (mini_string_len(argv[4]) < need) {
        vm_error(vm, "gui-bitmap: data has %u bytes, %u needed (h rows of (w + 7) / 8)",
                 (unsigned)mini_string_len(argv[4]), (unsigned)need);
        return mini_void();
    }
    int bg = (argc > 7 && mini_is_fixnum(argv[7])) ? (int)(mini_to_fixnum(argv[7]) & 0xFFFF) : -1;
    mini_gui_bitmap((int)mini_to_fixnum(argv[0]), (int)mini_to_fixnum(argv[1]), (int)w, (int)h,
                    (const uint8_t *)mini_string_data(argv[4]), (int)scale, (uint16_t)mini_to_fixnum(argv[6]), bg);
    return mini_void();
}

/* The resolved path of an image argument (an app's folder), or NULL (error set) */
static bool image_path(MiniVM *vm, const char *who, Value v, char *out, size_t cap) {
    if (!mini_is_string(v)) {
        vm_error(vm, "%s: expected a file path string", who);
        return false;
    }
    return mini_access_file(vm, who, mini_string_data(v), false, out, cap);
}

/* (gui-image x y path [frame [speed]]) */
static Value prim_gui_image(MiniVM *vm, int argc, Value *argv) {
    if (argc < 3 || !mini_is_fixnum(argv[0]) || !mini_is_fixnum(argv[1])) {
        vm_error(vm, "gui-image: expected x y path [frame [speed]]");
        return mini_void();
    }
    char path[112], err[96];
    if (!image_path(vm, "gui-image", argv[2], path, sizeof(path))) return mini_void();
    int frame = -1;
    if (argc > 3) {
        if (mini_is_fixnum(argv[3]) && mini_to_fixnum(argv[3]) >= 0) frame = (int)mini_to_fixnum(argv[3]);
        else if (mini_is_false(argv[3])) frame = 0;
        else if (argv[3] != MINI_CONST_TRUE) {
            vm_error(vm, "gui-image: frame must be a frame number, #t (play) or #f (the first frame)");
            return mini_void();
        }
    }
    int speed = 100;
    if (argc > 4) {
        if (!mini_is_fixnum(argv[4]) || mini_to_fixnum(argv[4]) < 10 || mini_to_fixnum(argv[4]) > 1000) {
            vm_error(vm, "gui-image: speed must be 10..1000 (percent)");
            return mini_void();
        }
        speed = (int)mini_to_fixnum(argv[4]);
    }
    if (!gui_image_get(path, err, sizeof(err))) {
        vm_error(vm, "gui-image: %s", err);
        return mini_void();
    }
    mini_gui_image((int)mini_to_fixnum(argv[0]), (int)mini_to_fixnum(argv[1]), path, frame, speed);
    return mini_void();
}

/* (image-info path) => (width height frames total-ms), #f when the file is
 * missing or not an image this decoder reads */
static Value prim_image_info(MiniVM *vm, int argc, Value *argv) {
    char path[112], err[96];
    if (argc != 1 || !image_path(vm, "image-info", argv[0], path, sizeof(path))) {
        if (argc != 1) vm_error(vm, "image-info: expected a path");
        return mini_void();
    }
    MiniImage *im = gui_image_get(path, err, sizeof(err));
    if (!im) return mini_false();
    uint32_t total = 0;
    int n = mini_image_frames(im);
    for (int i = 0; n > 1 && i < n; i++) total += gui_image_delay(im, i, 100);
    Value items[4] = { mini_make_fixnum(mini_image_width(im)), mini_make_fixnum(mini_image_height(im)),
                       mini_make_fixnum(n), mini_make_fixnum((int64_t)total) };
    Value list = mini_nil();
    mini_push_root(vm, &list);
    for (int i = 3; i >= 0; i--) list = mini_make_cons(vm, items[i], list);
    mini_pop_root(vm);
    return list;
}

static Value prim_gui_chart(MiniVM *vm, int argc, Value *argv) {
    if (argc < 10 || !mini_is_vector(argv[4])) {
        vm_error(vm, "gui-chart: expected x y w h ring start count lo hi color");
        return mini_void();
    }
    for (int i = 0; i < 10; i++) {
        if (i != 4 && !mini_is_fixnum(argv[i])) {
            vm_error(vm, "gui-chart: argument %d must be an integer", i + 1);
            return mini_void();
        }
    }
    int x = (int)mini_to_fixnum(argv[0]), y = (int)mini_to_fixnum(argv[1]);
    int w = (int)mini_to_fixnum(argv[2]), h = (int)mini_to_fixnum(argv[3]);
    size_t len = mini_vector_length(argv[4]);
    int64_t start = mini_to_fixnum(argv[5]), count = mini_to_fixnum(argv[6]);
    int64_t lo = mini_to_fixnum(argv[7]), hi = mini_to_fixnum(argv[8]);
    if (h > 256) h = 256;
    if (count > (int64_t)len) count = (int64_t)len;
    if (count > 320) count = 320;
    if (count < 2 || len == 0 || hi <= lo) return mini_void();
    uint8_t ys[320];
    for (int64_t i = 0; i < count; i++) {
        Value v = mini_vector_ref(vm, argv[4], (size_t)((start + i) % (int64_t)len));
        int64_t n = mini_is_fixnum(v) ? mini_to_fixnum(v) : lo;
        if (n < lo) n = lo;
        if (n > hi) n = hi;
        ys[i] = (uint8_t)((h - 1) - (n - lo) * (h - 1) / (hi - lo));
    }
    mini_gui_chart(x, y, w, h, ys, (int)count, (uint16_t)mini_to_fixnum(argv[9]));
    return mini_void();
}

/* (gui-arc cx cy r thickness start-deg sweep-deg color) */
static Value prim_gui_arc(MiniVM *vm, int argc, Value *argv) {
    if (argc < 7) {
        vm_error(vm, "gui-arc: expected cx cy r thickness start-deg sweep-deg color");
        return mini_void();
    }
    for (int i = 0; i < 7; i++) {
        if (!mini_is_fixnum(argv[i])) {
            vm_error(vm, "gui-arc: argument %d must be an integer", i + 1);
            return mini_void();
        }
    }
    mini_gui_arc((int)mini_to_fixnum(argv[0]), (int)mini_to_fixnum(argv[1]), (int)mini_to_fixnum(argv[2]),
                 (int)mini_to_fixnum(argv[3]), (int)mini_to_fixnum(argv[4]), (int)mini_to_fixnum(argv[5]),
                 (uint16_t)mini_to_fixnum(argv[6]));
    return mini_void();
}

/* (gui-push-clip x y w h): save the clip, then clip to the rectangle */
static Value prim_gui_push_clip(MiniVM *vm, int argc, Value *argv) {
    if (argc < 4 || !mini_is_fixnum(argv[0]) || !mini_is_fixnum(argv[1]) ||
        !mini_is_fixnum(argv[2]) || !mini_is_fixnum(argv[3])) {
        vm_error(vm, "gui-push-clip: expected x y w h");
        return mini_void();
    }
    if (s_clip_depth >= GUI_CLIP_DEPTH) {
        vm_error(vm, "gui-push-clip: nested deeper than %d", GUI_CLIP_DEPTH);
        return mini_void();
    }
    int *slot = s_clip_stack[s_clip_depth++];
    mini_gui_get_clip(&slot[0], &slot[1], &slot[2], &slot[3]);
    mini_gui_set_clip((int)mini_to_fixnum(argv[0]), (int)mini_to_fixnum(argv[1]),
                      (int)mini_to_fixnum(argv[2]), (int)mini_to_fixnum(argv[3]));
    return mini_void();
}

/* (gui-pop-clip): restore the clip saved by the matching gui-push-clip */
static Value prim_gui_pop_clip(MiniVM *vm, int argc, Value *argv) {
    (void)argc; (void)argv;
    if (s_clip_depth == 0) {
        vm_error(vm, "gui-pop-clip: no saved clip");
        return mini_void();
    }
    int *slot = s_clip_stack[--s_clip_depth];
    mini_gui_set_clip(slot[0], slot[1], slot[2], slot[3]);
    return mini_void();
}

static Value prim_gui_reset_clip(MiniVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc; (void)argv;
    mini_gui_reset_clip();
    return mini_void();
}

static Value prim_gui_get_clip(MiniVM *vm, int argc, Value *argv) {
    (void)argc; (void)argv;
    int x, y, w, h;
    mini_gui_get_clip(&x, &y, &w, &h);
    Value vh = mini_make_fixnum(h);
    mini_push_root(vm, &vh);
    Value vw = mini_make_fixnum(w);
    mini_push_root(vm, &vw);
    Value vy = mini_make_fixnum(y);
    mini_push_root(vm, &vy);
    Value vx = mini_make_fixnum(x);
    mini_push_root(vm, &vx);

    Value list = mini_make_cons(vm, vh, mini_nil());
    mini_pop_root(vm);
    mini_push_root(vm, &list);
    list = mini_make_cons(vm, vw, list);
    list = mini_make_cons(vm, vy, list);
    list = mini_make_cons(vm, vx, list);
    mini_pop_root(vm);
    mini_pop_root(vm);
    mini_pop_root(vm);
    mini_pop_root(vm);
    return list;
}

static Value prim_gui_dirty_rect(MiniVM *vm, int argc, Value *argv) {
    (void)argc; (void)argv;
    int x, y, w, h;
    mini_gui_get_dirty_rect(&x, &y, &w, &h);
    Value vh = mini_make_fixnum(h);
    mini_push_root(vm, &vh);
    Value vw = mini_make_fixnum(w);
    mini_push_root(vm, &vw);
    Value vy = mini_make_fixnum(y);
    mini_push_root(vm, &vy);
    Value vx = mini_make_fixnum(x);
    mini_push_root(vm, &vx);

    Value list = mini_make_cons(vm, vh, mini_nil());
    mini_pop_root(vm);
    mini_push_root(vm, &list);
    list = mini_make_cons(vm, vw, list);
    list = mini_make_cons(vm, vy, list);
    list = mini_make_cons(vm, vx, list);
    mini_pop_root(vm);
    mini_pop_root(vm);
    mini_pop_root(vm);
    mini_pop_root(vm);
    return list;
}

static Value prim_gui_mark_dirty(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    if (argc >= 4 && mini_is_fixnum(argv[0]) && mini_is_fixnum(argv[1]) &&
        mini_is_fixnum(argv[2]) && mini_is_fixnum(argv[3])) {
        mini_gui_mark_dirty_rect((int)mini_to_fixnum(argv[0]),
                                 (int)mini_to_fixnum(argv[1]),
                                 (int)mini_to_fixnum(argv[2]),
                                 (int)mini_to_fixnum(argv[3]));
    }
    return mini_void();
}

static Value prim_gui_card(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    if (argc >= 7 && mini_is_fixnum(argv[0]) && mini_is_fixnum(argv[1]) &&
        mini_is_fixnum(argv[2]) && mini_is_fixnum(argv[3]) &&
        mini_is_fixnum(argv[4]) && mini_is_fixnum(argv[5]) && mini_is_fixnum(argv[6])) {
        mini_gui_card((int)mini_to_fixnum(argv[0]),
                      (int)mini_to_fixnum(argv[1]),
                      (int)mini_to_fixnum(argv[2]),
                      (int)mini_to_fixnum(argv[3]),
                      (int)mini_to_fixnum(argv[4]),
                      (uint16_t)mini_to_fixnum(argv[5]),
                      (uint16_t)mini_to_fixnum(argv[6]));
    }
    return mini_void();
}

static Value prim_gui_create_buffer(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    if (argc >= 2 && mini_is_fixnum(argv[0]) && mini_is_fixnum(argv[1])) {
        int w = (int)mini_to_fixnum(argv[0]);
        int h = (int)mini_to_fixnum(argv[1]);
        MiniSubBuffer *buf = mini_gui_create_buffer(w, h);
        if (!buf) return mini_make_fixnum(-1);
        for (int i = 1; i < MAX_SUB_BUFFERS; i++) {
            if (s_sub_buffers[i] == NULL) {
                s_sub_buffers[i] = buf;
                return mini_make_fixnum(i);
            }
        }
        mini_gui_destroy_buffer(buf);
        return mini_make_fixnum(-1);
    }
    return mini_make_fixnum(-1);
}

static Value prim_gui_destroy_buffer(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    if (argc >= 1 && mini_is_fixnum(argv[0])) {
        int id = (int)mini_to_fixnum(argv[0]);
        if (id > 0 && id < MAX_SUB_BUFFERS && s_sub_buffers[id]) {
            mini_gui_destroy_buffer(s_sub_buffers[id]);
            s_sub_buffers[id] = NULL;
        }
    }
    return mini_void();
}

static Value prim_gui_set_target(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    if (argc >= 1 && mini_is_fixnum(argv[0])) {
        int id = (int)mini_to_fixnum(argv[0]);
        if (id == 0) {
            mini_gui_reset_target();
        } else if (id > 0 && id < MAX_SUB_BUFFERS && s_sub_buffers[id]) {
            mini_gui_set_target(s_sub_buffers[id]);
        }
    }
    return mini_void();
}

static Value prim_gui_reset_target(MiniVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc; (void)argv;
    mini_gui_reset_target();
    return mini_void();
}

static Value prim_gui_blit_buffer(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    if (argc >= 3 && mini_is_fixnum(argv[0]) && mini_is_fixnum(argv[1]) && mini_is_fixnum(argv[2])) {
        int dx = (int)mini_to_fixnum(argv[0]);
        int dy = (int)mini_to_fixnum(argv[1]);
        int id = (int)mini_to_fixnum(argv[2]);
        if (id > 0 && id < MAX_SUB_BUFFERS && s_sub_buffers[id]) {
            mini_gui_blit_buffer(dx, dy, s_sub_buffers[id]);
        }
    }
    return mini_void();
}

static Value prim_gui_framebuffer_enable(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    if (argc >= 1) {
        bool enable = mini_is_true(argv[0]);
        mini_gui_set_fb_enabled(enable);
    }
    return mini_gui_is_fb_enabled() ? mini_true() : mini_false();
}

static Value prim_gui_framebuffer_enabled_p(MiniVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc; (void)argv;
    return mini_gui_is_fb_enabled() ? mini_true() : mini_false();
}

static Value prim_gui_color_depth(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    if (argc >= 1 && mini_is_fixnum(argv[0])) {
        int bpp = (int)mini_to_fixnum(argv[0]);
        mini_gui_set_color_depth(bpp);
    }
    return mini_make_fixnum(mini_gui_get_color_depth());
}

static Value prim_gui_framebuffer_ram(MiniVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc; (void)argv;
    return mini_make_fixnum((int)mini_gui_get_fb_ram());
}

static Value prim_gui_mono_colors(MiniVM *vm, int argc, Value *argv) {
    if (argc >= 2 && mini_is_fixnum(argv[0]) && mini_is_fixnum(argv[1])) {
        uint16_t fg = (uint16_t)mini_to_fixnum(argv[0]);
        uint16_t bg = (uint16_t)mini_to_fixnum(argv[1]);
        mini_gui_set_mono_colors(fg, bg);
    }
    uint16_t fg = 0, bg = 0;
    mini_gui_get_mono_colors(&fg, &bg);
    Value vbg = mini_make_fixnum((int)bg);
    mini_push_root(vm, &vbg);
    Value vfg = mini_make_fixnum((int)fg);
    mini_push_root(vm, &vfg);
    Value list = mini_make_cons(vm, vbg, mini_nil());
    mini_push_root(vm, &list);
    list = mini_make_cons(vm, vfg, list);
    mini_pop_root(vm);
    mini_pop_root(vm);
    mini_pop_root(vm);
    return list;
}

static Value prim_gui_palette_set_bang(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    if (argc >= 2 && mini_is_fixnum(argv[0]) && mini_is_fixnum(argv[1])) {
        int idx = (int)mini_to_fixnum(argv[0]);
        uint16_t c = (uint16_t)mini_to_fixnum(argv[1]);
        mini_gui_set_palette(idx, c);
    }
    return mini_void();
}

static Value prim_gui_palette_get(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    if (argc >= 1 && mini_is_fixnum(argv[0])) {
        int idx = (int)mini_to_fixnum(argv[0]);
        return mini_make_fixnum((int)mini_gui_get_palette(idx));
    }
    return mini_make_fixnum(0);
}

static const char *value_name(Value v) {
    if (mini_is_symbol(v)) return mini_symbol_name(v);
    if (mini_is_string(v)) return mini_string_data(v);
    return NULL;
}

/* (gui-palette-preset name) => #t when name is a known scheme (legacy names too) */
static Value prim_gui_palette_preset(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    int scheme = argc >= 1 ? mini_gui_scheme_from_name(value_name(argv[0])) : -1;
    if (scheme < 0) return mini_false();
    mini_gui_set_scheme((MiniGuiScheme)scheme);
    return mini_true();
}

/* (gui-color-scheme [name]) => current scheme symbol: color gray eink green amber cyan gameboy invert custom */
static Value prim_gui_color_scheme(MiniVM *vm, int argc, Value *argv) {
    if (argc >= 1) {
        int scheme = mini_gui_scheme_from_name(value_name(argv[0]));
        if (scheme < 0) {
            vm_error(vm, "gui-color-scheme: unknown scheme (color gray eink green amber cyan gameboy invert custom)");
            return mini_void();
        }
        mini_gui_set_scheme((MiniGuiScheme)scheme);
    }
    return mini_make_symbol(vm, mini_gui_scheme_name(mini_gui_get_scheme()));
}

/* (gui-color-bits [n]) => bits per stored pixel: 16, 8, 4, 2 or 1 */
static Value prim_gui_color_bits(MiniVM *vm, int argc, Value *argv) {
    if (argc >= 1) {
        int bits = mini_is_fixnum(argv[0]) ? (int)mini_to_fixnum(argv[0]) : 0;
        if (bits != 1 && bits != 2 && bits != 4 && bits != 8 && bits != 16) {
            vm_error(vm, "gui-color-bits: expected 16, 8, 4, 2 or 1");
            return mini_void();
        }
        mini_gui_set_color_bits(bits);
    }
    return mini_make_fixnum(mini_gui_get_color_bits());
}

static Value prim_gui_dirty_rect_count(MiniVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc; (void)argv;
    return mini_make_fixnum(mini_gui_get_dirty_rect_count());
}

static Value prim_gui_dirty_rects(MiniVM *vm, int argc, Value *argv) {
    (void)argc; (void)argv;
    int count = mini_gui_get_dirty_rect_count();
    Value res = mini_nil();
    mini_push_root(vm, &res);
    for (int i = count - 1; i >= 0; i--) {
        int x = 0, y = 0, w = 0, h = 0;
        if (mini_gui_get_dirty_rect_at(i, &x, &y, &w, &h)) {
            Value vh = mini_make_fixnum(h);
            mini_push_root(vm, &vh);
            Value vw = mini_make_fixnum(w);
            mini_push_root(vm, &vw);
            Value vy = mini_make_fixnum(y);
            mini_push_root(vm, &vy);
            Value vx = mini_make_fixnum(x);
            mini_push_root(vm, &vx);

            Value rect_list = mini_make_cons(vm, vh, mini_nil());
            mini_push_root(vm, &rect_list);
            rect_list = mini_make_cons(vm, vw, rect_list);
            rect_list = mini_make_cons(vm, vy, rect_list);
            rect_list = mini_make_cons(vm, vx, rect_list);

            res = mini_make_cons(vm, rect_list, res);

            mini_pop_root(vm); /* rect_list */
            mini_pop_root(vm); /* vx */
            mini_pop_root(vm); /* vy */
            mini_pop_root(vm); /* vw */
            mini_pop_root(vm); /* vh */
        }
    }
    mini_pop_root(vm); /* res */
    return res;
}

static Value prim_gui_flush_dirty(MiniVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc; (void)argv;
    if (s_gui_mode == MINI_GUI_MODE_FULL && s_flush_y1 > s_flush_y0) {
        gui_emit_rows(s_flush_y0, s_flush_y1, NULL, NULL);
        s_flush_y0 = s_flush_y1 = 0;
    }
#if !defined(__wasm__) && !defined(ESP_PLATFORM)
    /* Native simulators present the VFB instantly; the WASM canvas still
     * needs the dirty rects, so it keeps them until JS repaints. */
    mini_gui_clear_dirty();
#endif
    return mini_void();
}

typedef struct {
    MiniVM *vm;
    Value thunk;
    int nparams;
} GuiSliceThunk;

static void gui_slice_scheme(int idx, int y, int h, void *ud) {
    GuiSliceThunk *t = (GuiSliceThunk *)ud;
    if (mini_get_error(t->vm)) return;
    if (t->nparams >= 3) {
        Value args[3] = { mini_make_fixnum(idx), mini_make_fixnum(y), mini_make_fixnum(h) };
        mini_call(t->vm, t->thunk, 3, args);
    } else if (t->nparams == 1) {
        Value arg = mini_make_fixnum(idx);
        mini_call(t->vm, t->thunk, 1, &arg);
    } else {
        mini_call(t->vm, t->thunk, 0, NULL);
    }
}

/* (gui-render-bands band-h thunk): calls thunk for each slice of band-h rows;
 * a thunk taking 3 arguments gets (index y h), one taking 1 gets index */
static Value prim_gui_render_bands(MiniVM *vm, int argc, Value *argv) {
    if (s_rec_on) { s_rec_overflow = true; return mini_void(); }
    if (argc < 2 || !mini_is_fixnum(argv[0])) return mini_void();
    GuiSliceThunk t = { vm, argv[1], 0 };
    if (mini_is_closure(t.thunk)) {
        ClosureObj *clo = (ClosureObj *)UNTAG_PTR(t.thunk);
        if (mini_is_code(clo->code)) t.nparams = ((CodeObj *)UNTAG_PTR(clo->code))->num_params;
    }
    mini_push_root(vm, &t.thunk);
    gui_slices((int)mini_to_fixnum(argv[0]), gui_slice_scheme, &t);
    mini_pop_root(vm);
    return mini_true();
}

/* (gui-text-wrap x y w h str color [font [line-height]]) => lines drawn:
 * str broken into lines of width w (at spaces, or between CJK
 * characters), as many as fit in h; cut text ends with an ellipsis */
static Value prim_gui_text_wrap(MiniVM *vm, int argc, Value *argv) {
    if (argc < 6 || !mini_is_string(argv[4])) {
        vm_error(vm, "gui-text-wrap: expected x y w h str color [font [line-height]]");
        return mini_void();
    }
    for (int i = 0; i < 6; i++) {
        if (i != 4 && !mini_is_fixnum(argv[i])) {
            vm_error(vm, "gui-text-wrap: x y w h and color must be integers");
            return mini_void();
        }
    }
    int font = argc > 6 && mini_is_fixnum(argv[6]) ? (int)mini_to_fixnum(argv[6]) : s_current_font;
    int line_h = argc > 7 && mini_is_fixnum(argv[7]) ? (int)mini_to_fixnum(argv[7]) : 0;
    return mini_make_fixnum(mini_gui_text_wrap((int)mini_to_fixnum(argv[0]), (int)mini_to_fixnum(argv[1]),
                                               (int)mini_to_fixnum(argv[2]), (int)mini_to_fixnum(argv[3]),
                                               mini_string_data(argv[4]), (uint16_t)mini_to_fixnum(argv[5]),
                                               font, line_h));
}

/* (gui-text-scroll x y w h str color [font [speed [direction]]]): text that
 * does not fit scrolls in a loop; speed in pixels per second (default 30),
 * direction 'left (marquee, default) or 'up (wrapped lines) */
static Value prim_gui_text_scroll(MiniVM *vm, int argc, Value *argv) {
    if (argc < 6 || !mini_is_string(argv[4])) {
        vm_error(vm, "gui-text-scroll: expected x y w h str color [font [speed [direction]]]");
        return mini_void();
    }
    for (int i = 0; i < 6; i++) {
        if (i != 4 && !mini_is_fixnum(argv[i])) {
            vm_error(vm, "gui-text-scroll: x y w h and color must be integers");
            return mini_void();
        }
    }
    int font = argc > 6 && mini_is_fixnum(argv[6]) ? (int)mini_to_fixnum(argv[6]) : s_current_font;
    int speed = argc > 7 && mini_is_fixnum(argv[7]) ? (int)mini_to_fixnum(argv[7]) : 30;
    int dir = MINI_GUI_SCROLL_LEFT;
    if (argc > 8 && mini_is_symbol(argv[8]) && strcmp(mini_symbol_name(argv[8]), "up") == 0) dir = MINI_GUI_SCROLL_UP;
    mini_gui_text_scroll((int)mini_to_fixnum(argv[0]), (int)mini_to_fixnum(argv[1]),
                         (int)mini_to_fixnum(argv[2]), (int)mini_to_fixnum(argv[3]),
                         mini_string_data(argv[4]), (uint16_t)mini_to_fixnum(argv[5]), font,
                         speed < 0 ? 0 : (speed > 2000 ? 2000 : speed), dir);
    return mini_void();
}

/* (gui-text-fit x y w str color [font]): one line in a box w wide; a line
 * too long for it follows config.ini [text] marquee (cut with "…", or moved
 * by pixels, characters or box-wide pieces) */
static Value prim_gui_text_fit(MiniVM *vm, int argc, Value *argv) {
    if (argc < 5 || !mini_is_string(argv[3]) || !mini_is_fixnum(argv[0]) || !mini_is_fixnum(argv[1]) ||
        !mini_is_fixnum(argv[2]) || !mini_is_fixnum(argv[4])) {
        vm_error(vm, "gui-text-fit: expected x y w str color [font]");
        return mini_void();
    }
    int font = argc > 5 && mini_is_fixnum(argv[5]) ? (int)mini_to_fixnum(argv[5]) : s_current_font;
    const MiniFont *f = mini_font_get(font);
    int h = (f ? f->height : 16) + 2;
    mini_gui_text_scroll((int)mini_to_fixnum(argv[0]), (int)mini_to_fixnum(argv[1]), (int)mini_to_fixnum(argv[2]), h,
                         mini_string_data(argv[3]), (uint16_t)mini_to_fixnum(argv[4]), font, 0, MINI_GUI_SCROLL_FIT);
    return mini_void();
}

/* (gui-text-area x y w h str color [font [line-h]]): text wrapped to the box;
 * taller than the box it follows config.ini [text] wrap_marquee (cut with
 * "…", or rolled up by pixels, lines or box-full pages) */
static Value prim_gui_text_area(MiniVM *vm, int argc, Value *argv) {
    if (argc < 6 || !mini_is_string(argv[4])) {
        vm_error(vm, "gui-text-area: expected x y w h str color [font [line-h]]");
        return mini_void();
    }
    for (int i = 0; i < 6; i++) {
        if (i != 4 && !mini_is_fixnum(argv[i])) {
            vm_error(vm, "gui-text-area: x y w h and color must be integers");
            return mini_void();
        }
    }
    int font = argc > 6 && mini_is_fixnum(argv[6]) ? (int)mini_to_fixnum(argv[6]) : s_current_font;
    int line_h = argc > 7 && mini_is_fixnum(argv[7]) ? (int)mini_to_fixnum(argv[7]) : 0;
    mini_gui_text_scroll((int)mini_to_fixnum(argv[0]), (int)mini_to_fixnum(argv[1]), (int)mini_to_fixnum(argv[2]),
                         (int)mini_to_fixnum(argv[3]), mini_string_data(argv[4]), (uint16_t)mini_to_fixnum(argv[5]),
                         font, line_h < 0 ? 0 : (line_h > 255 ? 255 : line_h), MINI_GUI_SCROLL_AREA);
    return mini_void();
}

/* Refreshes per second as a fixnum, or a flonum when not whole */
static Value fps_value(MiniVM *vm, int fps_x10) {
    if (fps_x10 % 10 == 0) return mini_make_fixnum(fps_x10 / 10);
    return mini_make_flonum(vm, fps_x10 / 10.0);
}

/* (gui-marquee) => (mode fps step hold) of one-line text; (gui-marquee 'wrap)
 * of wrapped text; hold: seconds the start stays. (gui-marquee mode [fps
 * [step [hold]]]) / (gui-marquee 'wrap mode ...) set them until the next
 * boot (config.ini [text] keeps them) */
static Value prim_gui_marquee(MiniVM *vm, int argc, Value *argv) {
    int which = MINI_GUI_TEXT_LINE, a = 0;
    if (argc >= 1 && mini_is_symbol(argv[0]) && strcmp(mini_symbol_name(argv[0]), "wrap") == 0) {
        which = MINI_GUI_TEXT_WRAP;
        a = 1;
    }
    int mode, fps_x10, step;
    mini_gui_get_marquee(which, &mode, &fps_x10, &step);
    if (argc > a) {
        const char *name = mini_is_symbol(argv[a]) ? mini_symbol_name(argv[a])
                         : (mini_is_string(argv[a]) ? mini_string_data(argv[a]) : NULL);
        int m = mini_gui_marquee_parse(which, name);
        if (m < 0) {
            vm_error(vm, which == MINI_GUI_TEXT_WRAP ? "gui-marquee: wrapped text takes none, pixel, line or page"
                                                    : "gui-marquee: one-line text takes none, pixel, char or page");
            return mini_void();
        }
        if (argc > a + 1 && (mini_is_fixnum(argv[a + 1]) || mini_is_flonum(argv[a + 1]))) {
            double fps = mini_is_fixnum(argv[a + 1]) ? (double)mini_to_fixnum(argv[a + 1]) : mini_to_flonum(argv[a + 1]);
            fps_x10 = (int)(fps * 10.0 + 0.5);
        }
        if (argc > a + 2 && mini_is_fixnum(argv[a + 2])) step = (int)mini_to_fixnum(argv[a + 2]);
        if (argc > a + 3 && (mini_is_fixnum(argv[a + 3]) || mini_is_flonum(argv[a + 3]))) {
            double hold = mini_is_fixnum(argv[a + 3]) ? (double)mini_to_fixnum(argv[a + 3]) : mini_to_flonum(argv[a + 3]);
            mini_gui_set_marquee_hold(which, (int)(hold * 1000.0 + 0.5));
        }
        mini_gui_set_marquee(which, m, fps_x10, step);
        mini_gui_get_marquee(which, &mode, &fps_x10, &step);
    }
    Value name = mini_make_symbol(vm, mini_gui_marquee_name(mode));
    mini_push_root(vm, &name);
    Value fps = fps_value(vm, fps_x10);
    mini_push_root(vm, &fps);
    int hold_ms = mini_gui_get_marquee_hold(which);
    Value hold = hold_ms % 1000 == 0 ? mini_make_fixnum(hold_ms / 1000) : mini_make_flonum(vm, hold_ms / 1000.0);
    Value lst = mini_make_cons(vm, hold, mini_nil());
    mini_push_root(vm, &lst);
    lst = mini_make_cons(vm, mini_make_fixnum(step), lst);
    lst = mini_make_cons(vm, fps, lst);
    lst = mini_make_cons(vm, name, lst);
    mini_pop_root(vm);
    mini_pop_root(vm);
    mini_pop_root(vm);
    return lst;
}

/* (gui-seg7 x y digit-w digit-h thickness str on off glow highlight) => width */
static Value prim_gui_seg7(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    if (argc < 10 || !mini_is_string(argv[5])) return mini_make_fixnum(0);
    for (int i = 0; i < 10; i++) {
        if (i != 5 && !mini_is_fixnum(argv[i])) return mini_make_fixnum(0);
    }
    int w = mini_gui_seg7((int)mini_to_fixnum(argv[0]), (int)mini_to_fixnum(argv[1]),
                          (int)mini_to_fixnum(argv[2]), (int)mini_to_fixnum(argv[3]),
                          (int)mini_to_fixnum(argv[4]), mini_string_data(argv[5]),
                          (uint16_t)mini_to_fixnum(argv[6]), (uint16_t)mini_to_fixnum(argv[7]),
                          (uint16_t)mini_to_fixnum(argv[8]), (uint16_t)mini_to_fixnum(argv[9]));
    return mini_make_fixnum(w);
}

/* (gui-seg7-width digit-w thickness str) */
static Value prim_gui_seg7_width(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    if (argc < 3 || !mini_is_fixnum(argv[0]) || !mini_is_fixnum(argv[1]) || !mini_is_string(argv[2])) {
        return mini_make_fixnum(0);
    }
    return mini_make_fixnum(mini_gui_seg7_width((int)mini_to_fixnum(argv[0]), (int)mini_to_fixnum(argv[1]),
                                                mini_string_data(argv[2])));
}

/* (gui-visible? y h): do rows [y, y+h) intersect the current clip? Lets
 * render code skip whole sections outside the active band. */
static Value prim_gui_visible_p(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    if (argc < 2 || !mini_is_fixnum(argv[0]) || !mini_is_fixnum(argv[1])) return mini_true();
    int y = (int)mini_to_fixnum(argv[0]);
    int h = (int)mini_to_fixnum(argv[1]);
    return mini_make_bool(y < s_clip_y1 && y + h > s_clip_y0);
}

/* (gui-invalidate) | (gui-invalidate y h) | (gui-invalidate x y w h) */
static Value prim_gui_invalidate(MiniVM *vm, int argc, Value *argv) {
    (void)vm;
    if (argc >= 4 && mini_is_fixnum(argv[1]) && mini_is_fixnum(argv[3])) {
        mini_gui_invalidate((int)mini_to_fixnum(argv[1]), (int)mini_to_fixnum(argv[3]));
    } else if (argc >= 2 && mini_is_fixnum(argv[0]) && mini_is_fixnum(argv[1])) {
        mini_gui_invalidate((int)mini_to_fixnum(argv[0]), (int)mini_to_fixnum(argv[1]));
    } else {
        mini_gui_invalidate(0, s_height);
    }
    return mini_void();
}

static Value prim_gui_mode(MiniVM *vm, int argc, Value *argv) {
    (void)argc; (void)argv;
    switch (s_gui_mode) {
        case MINI_GUI_MODE_BAND:   return mini_make_symbol(vm, "band");
        case MINI_GUI_MODE_DIRECT: return mini_make_symbol(vm, "direct");
        case MINI_GUI_MODE_FULL:
        default:                   return mini_make_symbol(vm, "full");
    }
}

static Value prim_gui_band_lines(MiniVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc; (void)argv;
    return mini_make_fixnum(mini_gui_get_band_lines());
}

/* (gui-color-format) => panel bus format: rgb565 or rgb666 */
static Value prim_gui_color_format(MiniVM *vm, int argc, Value *argv) {
    (void)argc; (void)argv;
    return mini_make_symbol(vm, s_gui_color_format == MINI_GUI_COLOR_RGB666 ? "rgb666" : "rgb565");
}

static Value prim_gui_buffer_ram(MiniVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc; (void)argv;
    return mini_make_fixnum((int64_t)mini_gui_get_fb_ram());
}

/* (lcd-dma-ram) => bytes of the LCD driver's DMA copy buffer, 0 without DMA */
static Value prim_lcd_dma_ram(MiniVM *vm, int argc, Value *argv) {
    (void)vm; (void)argc; (void)argv;
#if defined(ESP_PLATFORM)
    return mini_make_fixnum((int64_t)esp32_ili9488_dma_ram_bytes());
#else
    return mini_make_fixnum(0);
#endif
}

void mini_register_gui_lib(MiniVM *vm) {
    if (!vm) return;
    static const MiniPrimEntry funcs[] = {
        { "gui-init",        prim_gui_init },
        { "gui-clear",       prim_gui_clear },
        { "gui-flush",       prim_gui_flush },
        { "gui-pixel",       prim_gui_pixel },
        { "gui-line",        prim_gui_line },
        { "gui-rect",        prim_gui_rect },
        { "gui-round-rect",  prim_gui_round_rect },
        { "gui-circle",      prim_gui_circle },
        { "gui-text",        prim_gui_text },
        { "gui-set-font",    prim_gui_set_font },
        { "gui-get-font",    prim_gui_get_font },
        { "gui-text-width",  prim_gui_text_width },
        { "gui-text-styled", prim_gui_text_styled },
        { "gui-text-box",    prim_gui_text_box },
        { "gui-rgb",         prim_gui_rgb },
        { "gui-width",       prim_gui_width },
        { "gui-height",      prim_gui_height },
        { "gui-set-resolution", prim_gui_set_resolution },
        { "gui-touch-poll",  prim_gui_touch_poll },
        { "gui-touch-down?", prim_gui_touch_down_p },
        /* Hardware Peripherals */
        { "ws2812-set",      prim_ws2812_set },
        { "ws2812-get",      prim_ws2812_get },
        { "ws2812-clear",    prim_ws2812_clear },
        { "light-read",      prim_light_read },
        { "light-lux",       prim_light_lux },
        { "backlight-set",    prim_backlight_set },
        { "backlight-auto",   prim_backlight_auto },
        { "backlight-get",    prim_backlight_get },
        { "backlight-offset", prim_backlight_offset },
        { "backlight-min",    prim_backlight_min },
        { "backlight-max",    prim_backlight_max },
        { "button-read",     prim_button_read },
        { "button-pressed?", prim_button_pressed_p },
        { "display-model",      prim_display_model },
        { "display-rotation",   prim_display_rotation },
        { "gui-rotation",       prim_display_rotation },
        { "gui-set-clip",        prim_gui_set_clip },
        { "gui-reset-clip",      prim_gui_reset_clip },
        { "gui-get-clip",        prim_gui_get_clip },
        { "gui-push-clip",       prim_gui_push_clip },
        { "gui-chart",           prim_gui_chart },
        { "gui-bitmap",          prim_gui_bitmap },
        { "gui-image",           prim_gui_image },
        { "image-info",          prim_image_info },
        { "gui-arc",             prim_gui_arc },
        { "gui-pop-clip",        prim_gui_pop_clip },
        { "gui-dirty-rect",      prim_gui_dirty_rect },
        { "gui-mark-dirty",      prim_gui_mark_dirty },
        { "gui-dirty-rect-count", prim_gui_dirty_rect_count },
        { "gui-dirty-rects",      prim_gui_dirty_rects },
        { "gui-flush-dirty",      prim_gui_flush_dirty },
        { "gui-render-bands",     prim_gui_render_bands },
        { "gui-card",            prim_gui_card },
        { "gui-create-buffer",   prim_gui_create_buffer },
        { "gui-destroy-buffer",  prim_gui_destroy_buffer },
        { "gui-set-target",      prim_gui_set_target },
        { "gui-reset-target",    prim_gui_reset_target },
        { "gui-blit-buffer",     prim_gui_blit_buffer },
        /* Framebuffer & Palette Management */
        { "gui-framebuffer-enable",   prim_gui_framebuffer_enable },
        { "gui-framebuffer-enabled?", prim_gui_framebuffer_enabled_p },
        { "gui-color-depth",          prim_gui_color_depth },
        { "gui-framebuffer-ram",      prim_gui_framebuffer_ram },
        { "gui-mono-colors",          prim_gui_mono_colors },
        { "gui-palette-set!",         prim_gui_palette_set_bang },
        { "gui-palette-get",          prim_gui_palette_get },
        { "gui-palette-preset",       prim_gui_palette_preset },
        { "gui-color-scheme",         prim_gui_color_scheme },
        { "gui-color-bits",           prim_gui_color_bits },
        { "gui-invalidate",           prim_gui_invalidate },
        { "gui-visible?",             prim_gui_visible_p },
        { "gui-seg7",                 prim_gui_seg7 },
        { "gui-text-scroll",          prim_gui_text_scroll },
        { "gui-text-fit",             prim_gui_text_fit },
        { "gui-text-area",            prim_gui_text_area },
        { "gui-marquee",              prim_gui_marquee },
        { "gui-text-wrap",            prim_gui_text_wrap },
        { "gui-seg7-width",           prim_gui_seg7_width },
        { "gui-mode",                 prim_gui_mode },
        { "gui-band-lines",           prim_gui_band_lines },
        { "gui-color-format",         prim_gui_color_format },
        { "gui-buffer-ram",           prim_gui_buffer_ram },
        { "lcd-dma-ram",              prim_lcd_dma_ram },
    };
    mini_register_prim_table(vm, funcs, sizeof(funcs) / sizeof(funcs[0]));
    mini_register_font_lib(vm);
}

/* The GUI's heap blocks, for the heap map */
void mini_gui_mem_owners(MiniMemOwnerFn fn, void *user) {
    for (int i = 0; i < s_surf.nchunks; i++) fn(s_surf.chunk[i], "gui band buffer", user);
    fn(s_dl_base_cmds, "gui display list", user);
    if (s_dl_cmds != s_dl_base_cmds) fn(s_dl_cmds, "gui display list (grown)", user);
    if (s_dl_strings != s_dl_base_strings) fn(s_dl_strings, "gui display strings (grown)", user);
    fn(s_wrap, "gui wrap table", user);
    fn(s_custom, "gui custom palette", user);
}
