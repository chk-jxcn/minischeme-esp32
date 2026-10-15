/**
 * @file minischeme_mirror.c
 * @brief Screen mirroring (format in minischeme_mirror.h).
 */

#include "minischeme_mirror.h"
#include "minischeme_gui.h"
#include "minischeme_internal.h"
#include <stdlib.h>
#include <string.h>

/* Each row's version (word memory: IRAM on the ESP32, read and written as
 * whole words); allocated with the first mark. Without it every row counts
 * as changed. */
static volatile uint32_t *s_ver = NULL;
static uint32_t s_clock = 1;   /* every row starts at 1: since 0 asks for the whole screen */
/* the shared cursor of viewers that keep none (mini_mirror_fill) */
static uint32_t s_old_since = 0, s_old_pass = 0;
static int s_old_from = 0;

static bool ver_table(void) {
    if (!s_ver) {
        s_ver = (volatile uint32_t *)mini_word_alloc(MINI_MIRROR_MAX_ROWS * sizeof(uint32_t));
        if (s_ver) mini_words_fill((void *)s_ver, s_clock, MINI_MIRROR_MAX_ROWS * sizeof(uint32_t));
    }
    return s_ver != NULL;
}

void mini_mirror_mark(int y, int h) {
    int y1 = y + h;
    if (y < 0) y = 0;
    if (y1 > MINI_MIRROR_MAX_ROWS) y1 = MINI_MIRROR_MAX_ROWS;
    if (y >= y1 || !ver_table()) return;
    uint32_t v = ++s_clock;
    for (int r = y; r < y1; r++) s_ver[r] = v;
}

void mini_mirror_mark_all(void) {
    mini_mirror_mark(0, MINI_MIRROR_MAX_ROWS);
}

uint32_t mini_mirror_clock(void) { return s_clock; }

void mini_mirror_mem_owners(void (*fn)(const void *block, const char *what, void *user), void *user) {
    if (s_ver) fn((const void *)s_ver, "mirror row versions", user);
}

size_t mini_mirror_row_max(int width) {
    return 4 + (size_t)width * 2 + (size_t)width / 128 + 1;
}

size_t mini_mirror_pack(const uint16_t *px, int count, uint8_t *out) {
    size_t n = 0;
    int i = 0;
    while (i < count) {
        int run = 1;
        while (i + run < count && run < 129 && px[i + run] == px[i]) run++;
        if (run >= 2) {
            out[n++] = (uint8_t)(run + 126);
            out[n++] = (uint8_t)(px[i] & 0xFF);
            out[n++] = (uint8_t)(px[i] >> 8);
            i += run;
            continue;
        }
        /* literals up to the next run of two (or 128 pixels) */
        int lit = 1;
        while (i + lit < count && lit < 128 && !(i + lit + 1 < count && px[i + lit] == px[i + lit + 1])) lit++;
        out[n++] = (uint8_t)(lit - 1);
        for (int k = 0; k < lit; k++) {
            out[n++] = (uint8_t)(px[i + k] & 0xFF);
            out[n++] = (uint8_t)(px[i + k] >> 8);
        }
        i += lit;
    }
    return n;
}

typedef struct {
    uint8_t *out;
    size_t cap, len;
    int rows;
    int height;
    bool full;          /* out of space: the rest waits for the next request */
    uint16_t *line;     /* one row of RGB565 */
    uint32_t since;     /* rows newer than this are sent */
    int lo, hi;         /* the span being captured (its bands hold more rows) */
    int last;           /* the last row sent */
} MirrorCtx;

static bool row_newer(int y, uint32_t since) { return !s_ver || s_ver[y] > since; }

static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

static void mirror_emit(const MiniGuiRows *rows, int y, int h, int width, void *userdata) {
    MirrorCtx *c = (MirrorCtx *)userdata;
    for (int i = 0; i < h && !c->full; i++) {
        int row = y + i;
        if (row < c->lo || row >= c->hi || row >= c->height || !row_newer(row, c->since)) continue;
        if (c->len + mini_mirror_row_max(width) > c->cap) {
            c->full = true;
            return;
        }
        mini_gui_rows_to_rgb565(rows, i, 0, width, c->line);
        size_t n = mini_mirror_pack(c->line, width, c->out + c->len + 4);
        wr16(c->out + c->len, (uint16_t)row);
        wr16(c->out + c->len + 2, (uint16_t)n);
        c->len += 4 + n;
        c->rows++;
        c->last = row;
    }
}

/* The rows at or below `from` newer than since, as many as fit; *next gets
 * the row to go on at, or -1 when none are left */
static size_t mirror_fill_rows(MiniVM *vm, uint8_t *out, size_t cap, uint32_t since, int from, int *next) {
    int width = mini_gui_get_width(), height = mini_gui_get_height();
    if (height > MINI_MIRROR_MAX_ROWS) height = MINI_MIRROR_MAX_ROWS;
    *next = -1;
    if (!out || cap < MINI_MIRROR_HEADER) return 0;
    ver_table();
    if (since > s_clock) since = 0;   /* a cursor from before a restart */
    if (from < 0) from = 0;
    MirrorCtx c = { out, cap, MINI_MIRROR_HEADER, 0, height, false, NULL, since, 0, 0, -1 };
    uint8_t flags = 0;
    if (mini_gui_get_mode() == MINI_GUI_MODE_DIRECT) {
        flags |= MINI_MIRROR_NOPIXELS;
    } else if (width > 0 && (c.line = (uint16_t *)malloc((size_t)width * 2)) != NULL) {
        for (int y = from; y < height && !c.full; ) {
            if (!row_newer(y, since)) {
                y++;
                continue;
            }
            int end = y;
            while (end < height && row_newer(end, since)) end++;
            c.lo = y;
            c.hi = end;
            mini_gui_capture(vm, y, end, mirror_emit, &c);
            y = end;
        }
        free(c.line);
        if (c.full) {
            flags |= MINI_MIRROR_MORE;
            *next = c.last + 1;
        }
    }
    memcpy(out, "MSCR", 4);
    out[4] = 2;
    out[5] = flags;
    wr16(out + 6, (uint16_t)width);
    wr16(out + 8, (uint16_t)height);
    wr16(out + 10, (uint16_t)c.rows);
    out[12] = (uint8_t)s_clock;
    out[13] = (uint8_t)(s_clock >> 8);
    out[14] = (uint8_t)(s_clock >> 16);
    out[15] = (uint8_t)(s_clock >> 24);
    return c.len;
}

size_t mini_mirror_fill_since(MiniVM *vm, uint8_t *out, size_t cap, uint32_t since, int from) {
    int next;
    return mirror_fill_rows(vm, out, cap, since, from, &next);
}

size_t mini_mirror_fill(MiniVM *vm, uint8_t *out, size_t cap) {
    if (s_old_from == 0) s_old_pass = s_clock;   /* a pass starts */
    int next;
    size_t n = mirror_fill_rows(vm, out, cap, s_old_since, s_old_from, &next);
    if (next >= 0) {
        s_old_from = next;
    } else {
        s_old_since = s_old_pass;
        s_old_from = 0;
    }
    return n;
}

void mini_mirror_restart(void) {
    s_old_since = 0;
    s_old_from = 0;
}
