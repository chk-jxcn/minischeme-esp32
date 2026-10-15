/**
 * @file minischeme_image.c
 * @brief Baseline JPEG decoding by MCU rows, with random access to the rows
 *        through recorded entropy-decoder states; MANI animations of JPEG
 *        frames (format in minischeme_image.h).
 */

#include "minischeme_image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ========================================================================= */
/* I/O                                                                        */
/* ========================================================================= */

#if !defined(MINI_IMAGE_NO_STDIO)
static void *std_open(const char *path) { return fopen(path, "rb"); }
static int std_read(void *f, void *buf, int n) { return (int)fread(buf, 1, (size_t)n, (FILE *)f); }
static bool std_seek(void *f, long off) { return fseek((FILE *)f, off, SEEK_SET) == 0; }
static void std_close(void *f) { fclose((FILE *)f); }
static MiniImageIO s_io = { std_open, std_read, std_seek, std_close };
#else
static MiniImageIO s_io = { NULL, NULL, NULL, NULL };
#endif

void mini_image_set_io(const MiniImageIO *io) {
    if (io) s_io = *io;
}

/* A buffered byte source over the file */
typedef struct {
    void *f;
    long base;          /* file offset of buf[0] */
    int pos, len;
    uint8_t buf[512];
} Src;

static bool src_seek(Src *s, long off) {
    s->pos = s->len = 0;
    s->base = off;
    return s_io.seek(s->f, off);
}

static int src_byte(Src *s) {
    if (s->pos >= s->len) {
        s->base += s->len;
        s->pos = 0;
        s->len = s_io.read(s->f, s->buf, (int)sizeof(s->buf));
        if (s->len <= 0) {
            s->len = 0;
            return -1;
        }
    }
    return s->buf[s->pos++];
}

static long src_tell(const Src *s) { return s->base + s->pos; }

static int src_u16be(Src *s) {
    int a = src_byte(s), b = src_byte(s);
    return (a < 0 || b < 0) ? -1 : (a << 8 | b);
}

static bool src_read(Src *s, uint8_t *out, int n) {
    for (int i = 0; i < n; i++) {
        int b = src_byte(s);
        if (b < 0) return false;
        out[i] = (uint8_t)b;
    }
    return true;
}

/* ========================================================================= */
/* Decoder state                                                              */
/* ========================================================================= */

typedef struct {
    uint16_t fast[256];   /* the next 8 bits -> length << 8 | value; 0: a longer code */
    int32_t maxcode[18];  /* codes of length l are below maxcode[l] */
    int32_t valoff[17];   /* index into vals of code c of length l: c + valoff[l] */
    uint8_t vals[256];
    bool present;
} Huff;

typedef struct {
    int id, h, v, tq, td, ta;
} Comp;

/* Where an MCU row starts: everything the entropy decoder carries */
typedef struct {
    long pos;
    uint32_t acc;
    int8_t nbits;
    uint8_t marker;
    int16_t pred[3];
    uint16_t togo;
} Snap;

struct MiniImage {
    Src src;
    int w, h, frames;
    uint32_t *off;            /* per frame: file offset of its JPEG */
    uint16_t *delay;          /* per frame: ms */
    int frames_cap, snaps_cap;  /* the arrays' room, kept when the decoder is reused */
    int frame;                /* the frame whose headers are loaded, -1 */

    Huff dc[2], ac[2];
    uint16_t q[4][64];        /* in zigzag order */
    Comp comp[3];
    int ncomp, hmax, vmax, mcu_w, mcu_h, mcus_x, mcus_y, ri;

    uint32_t acc;             /* bits, the next one at nbits - 1 */
    int nbits;
    int marker;               /* a marker met in the entropy data (0: none) */
    int pred[3];
    int togo;                 /* MCUs to the next restart marker */
    Snap *snaps;              /* MCU row starts, rows 0..known */
    int known;
    int next_row;             /* the MCU row the state is at */

    int32_t blk[64];
    uint8_t plane[3][256];    /* one MCU of each component */
    uint16_t px[256];         /* one MCU in RGB565 */
};

static const uint8_t DEZIG[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21,
    28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61,
    54, 47, 55, 62, 63,
};

static void set_err(char *err, size_t cap, const char *msg) {
    if (err && cap) snprintf(err, cap, "%s", msg);
}

/* ========================================================================= */
/* Headers                                                                    */
/* ========================================================================= */

static bool huff_build(Huff *h, const uint8_t counts[16], const uint8_t *vals, int nvals) {
    memcpy(h->vals, vals, (size_t)nvals);
    memset(h->fast, 0, sizeof(h->fast));
    int code = 0, k = 0;
    for (int len = 1; len <= 16; len++) {
        h->valoff[len] = k - code;
        for (int i = 0; i < counts[len - 1]; i++, k++, code++) {
            if (len <= 8) {
                int shift = 8 - len;
                for (int j = 0; j < (1 << shift); j++) h->fast[(code << shift) | j] = (uint16_t)(len << 8 | vals[k]);
            }
        }
        if (code > (1 << len)) return false;
        h->maxcode[len] = code;
        code <<= 1;
    }
    h->maxcode[17] = 0x7FFFFFFF;
    h->present = true;
    return true;
}

/* Reads the headers of the JPEG at the source's position up to its scan */
static bool parse_jpeg(MiniImage *im, char *err, size_t cap) {
    Src *s = &im->src;
    if (src_byte(s) != 0xFF || src_byte(s) != 0xD8) {
        set_err(err, cap, "not a JPEG file");
        return false;
    }
    bool sof = false;
    im->ri = 0;
    for (;;) {
        int b = src_byte(s);
        while (b == 0xFF) b = src_byte(s);   /* fill bytes before a marker */
        if (b < 0 || b == 0xD9) {
            set_err(err, cap, "JPEG ends before its image data");
            return false;
        }
        int len = src_u16be(s);
        if (len < 2) {
            set_err(err, cap, "bad JPEG segment");
            return false;
        }
        len -= 2;
        if (b == 0xC0 || b == 0xC1) {   /* baseline / extended sequential, Huffman */
            uint8_t hd[6];
            if (len < 6 || !src_read(s, hd, 6) || hd[0] != 8) {
                set_err(err, cap, "unsupported JPEG precision");
                return false;
            }
            int h = hd[1] << 8 | hd[2], w = hd[3] << 8 | hd[4];
            im->ncomp = hd[5];
            if ((im->ncomp != 1 && im->ncomp != 3) || len != 6 + 3 * im->ncomp || w <= 0 || h <= 0) {
                set_err(err, cap, "unsupported JPEG components");
                return false;
            }
            if (im->w == 0) {
                im->w = w;
                im->h = h;
            } else if (w != im->w || h != im->h) {
                set_err(err, cap, "an animation frame differs in size");
                return false;
            }
            im->hmax = im->vmax = 1;
            for (int i = 0; i < im->ncomp; i++) {
                uint8_t c[3];
                if (!src_read(s, c, 3)) return false;
                Comp *k = &im->comp[i];
                k->id = c[0];
                k->h = c[1] >> 4;
                k->v = c[1] & 15;
                k->tq = c[2] & 3;
                if (k->h < 1 || k->h > 2 || k->v < 1 || k->v > 2) {
                    set_err(err, cap, "unsupported JPEG sampling");
                    return false;
                }
                if (k->h > im->hmax) im->hmax = k->h;
                if (k->v > im->vmax) im->vmax = k->v;
            }
            if (im->ncomp == 1) im->comp[0].h = im->comp[0].v = im->hmax = im->vmax = 1;
            im->mcu_w = 8 * im->hmax;
            im->mcu_h = 8 * im->vmax;
            im->mcus_x = (w + im->mcu_w - 1) / im->mcu_w;
            im->mcus_y = (h + im->mcu_h - 1) / im->mcu_h;
            sof = true;
        } else if (b >= 0xC2 && b <= 0xCF && b != 0xC4 && b != 0xC8 && b != 0xCC) {
            set_err(err, cap, "progressive or arithmetic JPEG: convert it to baseline (the album's web page does)");
            return false;
        } else if (b == 0xC4) {        /* DHT */
            while (len > 0) {
                int tc = src_byte(s);
                uint8_t counts[16], vals[256];
                if (tc < 0 || !src_read(s, counts, 16)) return false;
                int n = 0;
                for (int i = 0; i < 16; i++) n += counts[i];
                if (n > 256 || (tc >> 4) > 1 || (tc & 15) > 1 || !src_read(s, vals, n)) {
                    set_err(err, cap, "bad JPEG Huffman table");
                    return false;
                }
                Huff *h = (tc >> 4) ? &im->ac[tc & 15] : &im->dc[tc & 15];
                if (!huff_build(h, counts, vals, n)) {
                    set_err(err, cap, "bad JPEG Huffman table");
                    return false;
                }
                len -= 17 + n;
            }
        } else if (b == 0xDB) {        /* DQT */
            while (len > 0) {
                int pq = src_byte(s);
                if (pq < 0 || (pq & 15) > 3) return false;
                for (int i = 0; i < 64; i++) {
                    int v = (pq >> 4) ? src_u16be(s) : src_byte(s);
                    if (v < 0) return false;
                    im->q[pq & 15][i] = (uint16_t)v;
                }
                len -= 1 + ((pq >> 4) ? 128 : 64);
            }
        } else if (b == 0xDD) {        /* DRI */
            im->ri = src_u16be(s);
            if (len != 2 || im->ri < 0) return false;
        } else if (b == 0xDA) {        /* SOS: the image data follows */
            int ns = src_byte(s);
            if (!sof || ns != im->ncomp || len != 4 + 2 * ns) {
                set_err(err, cap, "unsupported JPEG scan (one interleaved scan expected)");
                return false;
            }
            for (int i = 0; i < ns; i++) {
                int id = src_byte(s), t = src_byte(s);
                if (id != im->comp[i].id || t < 0) {
                    set_err(err, cap, "unsupported JPEG scan order");
                    return false;
                }
                im->comp[i].td = (t >> 4) & 1;
                im->comp[i].ta = t & 1;
                if (!im->dc[im->comp[i].td].present || !im->ac[im->comp[i].ta].present) {
                    set_err(err, cap, "JPEG scan without its Huffman tables");
                    return false;
                }
            }
            uint8_t rest[3];
            if (!src_read(s, rest, 3)) return false;
            return true;
        } else {                       /* APPn, COM, ...: skipped */
            for (int i = 0; i < len; i++) {
                if (src_byte(s) < 0) return false;
            }
        }
    }
}

/* ========================================================================= */
/* Entropy decoding                                                           */
/* ========================================================================= */

static int stream_byte(MiniImage *im) {
    if (im->marker) return 0;
    int b = src_byte(&im->src);
    if (b < 0) {
        im->marker = 0xD9;
        return 0;
    }
    if (b != 0xFF) return b;
    int c = src_byte(&im->src);
    while (c == 0xFF) c = src_byte(&im->src);
    if (c == 0) return 0xFF;   /* a stuffed 0xFF */
    im->marker = c < 0 ? 0xD9 : c;
    return 0;
}

static void fill(MiniImage *im) {
    while (im->nbits <= 24) {
        im->acc = (im->acc << 8) | (uint32_t)stream_byte(im);
        im->nbits += 8;
    }
}

static int get_bits(MiniImage *im, int n) {
    fill(im);
    int v = (int)((im->acc >> (im->nbits - n)) & ((1u << n) - 1u));
    im->nbits -= n;
    return v;
}

static int extend(int v, int s) {
    return v < (1 << (s - 1)) ? v - (1 << s) + 1 : v;
}

static int huff_decode(MiniImage *im, const Huff *h) {
    fill(im);
    uint16_t e = h->fast[(im->acc >> (im->nbits - 8)) & 0xFF];
    if (e) {
        im->nbits -= e >> 8;
        return e & 0xFF;
    }
    for (int len = 9; len <= 16; len++) {
        int c = (int)((im->acc >> (im->nbits - len)) & ((1u << len) - 1u));
        if (c < h->maxcode[len]) {
            im->nbits -= len;
            int k = c + h->valoff[len];
            return (k >= 0 && k < 256) ? h->vals[k] : -1;
        }
    }
    return -1;
}

/* One block of component ci; blk NULL: only past it */
static bool decode_block(MiniImage *im, int ci, int32_t *blk) {
    const Comp *c = &im->comp[ci];
    const uint16_t *q = im->q[c->tq];
    int t = huff_decode(im, &im->dc[c->td]);
    if (t < 0 || t > 11) return false;
    int diff = t ? extend(get_bits(im, t), t) : 0;
    im->pred[ci] += diff;
    if (blk) {
        memset(blk, 0, 64 * sizeof(int32_t));
        blk[0] = im->pred[ci] * q[0];
    }
    for (int k = 1; k < 64;) {
        int rs = huff_decode(im, &im->ac[c->ta]);
        if (rs < 0) return false;
        int r = rs >> 4, s = rs & 15;
        if (s == 0) {
            if (r != 15) break;   /* end of block */
            k += 16;
            continue;
        }
        k += r;
        if (k > 63) return false;
        int v = extend(get_bits(im, s), s);
        if (blk) blk[DEZIG[k]] = v * q[k];
        k++;
    }
    return true;
}

/* libjpeg's accurate integer IDCT (jidctint.c, "islow"): blk -> 8x8 samples */
#define CB 13
#define P1 2
#define DESC(x, n) (((x) + (1 << ((n) - 1))) >> (n))
static void idct(const int32_t *in, uint8_t *out, int stride) {
    int32_t ws[64];
    for (int c = 0; c < 8; c++) {
        const int32_t *p = in + c;
        int32_t *w = ws + c;
        if (!p[8] && !p[16] && !p[24] && !p[32] && !p[40] && !p[48] && !p[56]) {
            int32_t dc = p[0] * (1 << P1);
            for (int r = 0; r < 8; r++) w[r * 8] = dc;
            continue;
        }
        int32_t z2 = p[16], z3 = p[48];
        int32_t z1 = (z2 + z3) * 4433;
        int32_t t2 = z1 + z3 * -15137, t3 = z1 + z2 * 6270;
        z2 = p[0];
        z3 = p[32];
        int32_t t0 = (z2 + z3) * (1 << CB), t1 = (z2 - z3) * (1 << CB);
        int32_t t10 = t0 + t3, t13 = t0 - t3, t11 = t1 + t2, t12 = t1 - t2;
        t0 = p[56]; t1 = p[40]; t2 = p[24]; t3 = p[8];
        z1 = t0 + t3; z2 = t1 + t2; z3 = t0 + t2;
        int32_t z4 = t1 + t3, z5 = (z3 + z4) * 9633;
        t0 *= 2446; t1 *= 16819; t2 *= 25172; t3 *= 12299;
        z1 *= -7373; z2 *= -20995; z3 *= -16069; z4 *= -3196;
        z3 += z5; z4 += z5;
        t0 += z1 + z3; t1 += z2 + z4; t2 += z2 + z3; t3 += z1 + z4;
        w[0] = DESC(t10 + t3, CB - P1);  w[56] = DESC(t10 - t3, CB - P1);
        w[8] = DESC(t11 + t2, CB - P1);  w[48] = DESC(t11 - t2, CB - P1);
        w[16] = DESC(t12 + t1, CB - P1); w[40] = DESC(t12 - t1, CB - P1);
        w[24] = DESC(t13 + t0, CB - P1); w[32] = DESC(t13 - t0, CB - P1);
    }
    for (int r = 0; r < 8; r++) {
        const int32_t *w = ws + r * 8;
        uint8_t *o = out + r * stride;
        int32_t z2 = w[2], z3 = w[6];
        int32_t z1 = (z2 + z3) * 4433;
        int32_t t2 = z1 + z3 * -15137, t3 = z1 + z2 * 6270;
        int32_t t0 = (w[0] + w[4]) * (1 << CB), t1 = (w[0] - w[4]) * (1 << CB);
        int32_t t10 = t0 + t3, t13 = t0 - t3, t11 = t1 + t2, t12 = t1 - t2;
        t0 = w[7]; t1 = w[5]; t2 = w[3]; t3 = w[1];
        z1 = t0 + t3; z2 = t1 + t2; z3 = t0 + t2;
        int32_t z4 = t1 + t3, z5 = (z3 + z4) * 9633;
        t0 *= 2446; t1 *= 16819; t2 *= 25172; t3 *= 12299;
        z1 *= -7373; z2 *= -20995; z3 *= -16069; z4 *= -3196;
        z3 += z5; z4 += z5;
        t0 += z1 + z3; t1 += z2 + z4; t2 += z2 + z3; t3 += z1 + z4;
        int32_t v[8] = { t10 + t3, t11 + t2, t12 + t1, t13 + t0, t13 - t0, t12 - t1, t11 - t2, t10 - t3 };
        for (int i = 0; i < 8; i++) {
            int32_t x = DESC(v[i], CB + P1 + 3) + 128;
            o[i] = (uint8_t)(x < 0 ? 0 : (x > 255 ? 255 : x));
        }
    }
}

static uint8_t clamp255(int32_t x) { return (uint8_t)(x < 0 ? 0 : (x > 255 ? 255 : x)); }

/* The current MCU's planes -> RGB565 */
static void mcu_to_rgb(MiniImage *im) {
    int mw = im->mcu_w, mh = im->mcu_h;
    if (im->ncomp == 1) {
        for (int i = 0; i < 64; i++) {
            uint8_t y = im->plane[0][i];
            im->px[i] = (uint16_t)((y & 0xF8) << 8 | (y & 0xFC) << 3 | y >> 3);
        }
        return;
    }
    const Comp *c = im->comp;
    int yw = 8 * c[0].h, bw = 8 * c[1].h, rw = 8 * c[2].h;
    for (int y = 0; y < mh; y++) {
        const uint8_t *yr = im->plane[0] + (y * c[0].v / im->vmax) * yw;
        const uint8_t *br = im->plane[1] + (y * c[1].v / im->vmax) * bw;
        const uint8_t *rr = im->plane[2] + (y * c[2].v / im->vmax) * rw;
        uint16_t *o = im->px + y * mw;
        for (int x = 0; x < mw; x++) {
            int32_t Y = (int32_t)yr[x * c[0].h / im->hmax] << 16;
            int32_t cb = br[x * c[1].h / im->hmax] - 128, cr = rr[x * c[2].h / im->hmax] - 128;
            uint8_t r = clamp255((Y + 91881 * cr + 32768) >> 16);
            uint8_t g = clamp255((Y - 22554 * cb - 46802 * cr + 32768) >> 16);
            uint8_t b = clamp255((Y + 116130 * cb + 32768) >> 16);
            o[x] = (uint16_t)((r & 0xF8) << 8 | (g & 0xFC) << 3 | b >> 3);
        }
    }
}

static void restart(MiniImage *im) {
    im->acc = 0;
    im->nbits = 0;
    if (!(im->marker >= 0xD0 && im->marker <= 0xD7)) {
        /* the marker is still ahead: find it */
        int b;
        while ((b = src_byte(&im->src)) >= 0) {
            if (b != 0xFF) continue;
            int c = src_byte(&im->src);
            while (c == 0xFF) c = src_byte(&im->src);
            if (c >= 0xD0 && c <= 0xD7) break;
        }
    }
    im->marker = 0;
    im->pred[0] = im->pred[1] = im->pred[2] = 0;
    im->togo = im->ri;
}

static void snap_take(MiniImage *im, Snap *s) {
    s->pos = src_tell(&im->src);
    s->acc = im->acc;
    s->nbits = (int8_t)im->nbits;
    s->marker = (uint8_t)im->marker;
    for (int i = 0; i < 3; i++) s->pred[i] = (int16_t)im->pred[i];
    s->togo = (uint16_t)im->togo;
}

static bool snap_restore(MiniImage *im, const Snap *s) {
    if (!src_seek(&im->src, s->pos)) return false;
    im->acc = s->acc;
    im->nbits = s->nbits;
    im->marker = s->marker;
    for (int i = 0; i < 3; i++) im->pred[i] = s->pred[i];
    im->togo = s->togo;
    return true;
}

/* Decodes MCU row `row` (the state is at its start); with fn, the MCUs in
 * columns [x0, x1) go out, rows [y0, y1) of them */
static bool decode_row(MiniImage *im, int row, int x0, int x1, int y0, int y1, MiniImageRowFn fn, void *ud) {
    int top = row * im->mcu_h;
    for (int mx = 0; mx < im->mcus_x; mx++) {
        int left = mx * im->mcu_w;
        bool show = fn && left < x1 && left + im->mcu_w > x0;
        for (int ci = 0; ci < im->ncomp; ci++) {
            const Comp *c = &im->comp[ci];
            int pw = 8 * c->h;
            for (int by = 0; by < c->v; by++) {
                for (int bx = 0; bx < c->h; bx++) {
                    if (!decode_block(im, ci, show ? im->blk : NULL)) return false;
                    if (show) idct(im->blk, im->plane[ci] + by * 8 * pw + bx * 8, pw);
                }
            }
        }
        if (show) {
            mcu_to_rgb(im);
            int cx0 = left > x0 ? left : x0, cx1 = left + im->mcu_w < x1 ? left + im->mcu_w : x1;
            if (cx1 > im->w) cx1 = im->w;
            for (int y = top; y < top + im->mcu_h; y++) {
                if (y < y0 || y >= y1 || y >= im->h || cx0 >= cx1) continue;
                fn(ud, cx0, y, im->px + (y - top) * im->mcu_w + (cx0 - left), cx1 - cx0);
            }
        }
        if (im->ri && --im->togo == 0 && (row < im->mcus_y - 1 || mx < im->mcus_x - 1)) restart(im);
    }
    return true;
}

/* Loads frame f's headers and puts the state at its first MCU row */
static bool load_frame(MiniImage *im, int f, char *err, size_t cap) {
    if (im->frame == f) return true;
    im->frame = -1;
    if (!src_seek(&im->src, (long)im->off[f])) {
        set_err(err, cap, "cannot seek in the image file");
        return false;
    }
    memset(im->dc, 0, sizeof(im->dc));
    memset(im->ac, 0, sizeof(im->ac));
    if (!parse_jpeg(im, err, cap)) return false;
    if (im->mcus_y + 1 > im->snaps_cap) {
        free(im->snaps);
        im->snaps = (Snap *)malloc(sizeof(Snap) * (size_t)(im->mcus_y + 1));
        im->snaps_cap = im->snaps ? im->mcus_y + 1 : 0;
        if (!im->snaps) {
            set_err(err, cap, "out of memory");
            return false;
        }
    }
    im->acc = 0;
    im->nbits = 0;
    im->marker = 0;
    im->pred[0] = im->pred[1] = im->pred[2] = 0;
    im->togo = im->ri;
    snap_take(im, &im->snaps[0]);
    im->known = 0;
    im->next_row = 0;
    im->frame = f;
    return true;
}

/* ========================================================================= */
/* Public                                                                     */
/* ========================================================================= */

/* The last closed decoder, kept with its arrays: opening the next image
 * allocates nothing (and the WASM build's allocator never frees) */
static MiniImage *s_spare = NULL;

/* Room for n frames in the frame table */
static bool frames_room(MiniImage *im, int n) {
    if (n <= im->frames_cap) return true;
    free(im->off);
    free(im->delay);
    im->off = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)n);
    im->delay = (uint16_t *)malloc(sizeof(uint16_t) * (size_t)n);
    im->frames_cap = (im->off && im->delay) ? n : 0;
    return im->frames_cap > 0;
}

MiniImage *mini_image_open(const char *path, char *err, size_t cap) {
    if (!s_io.open) {
        set_err(err, cap, "no file access for images");
        return NULL;
    }
    MiniImage *im = s_spare;
    s_spare = NULL;
    if (im) {
        /* everything but the arrays starts over */
        uint32_t *off = im->off;
        uint16_t *delay = im->delay;
        Snap *snaps = im->snaps;
        int fc = im->frames_cap, sc = im->snaps_cap;
        memset(im, 0, sizeof(*im));
        im->off = off;
        im->delay = delay;
        im->snaps = snaps;
        im->frames_cap = fc;
        im->snaps_cap = sc;
    } else {
        im = (MiniImage *)calloc(1, sizeof(MiniImage));
    }
    if (!im) {
        set_err(err, cap, "out of memory");
        return NULL;
    }
    im->frame = -1;
    im->src.f = s_io.open(path);
    if (!im->src.f) {
        mini_image_close(im);
        set_err(err, cap, "cannot open the image file");
        return NULL;
    }
    uint8_t hd[12];
    bool ok = src_seek(&im->src, 0) && src_read(&im->src, hd, 4);
    if (ok && memcmp(hd, "MANI", 4) == 0) {
        ok = src_read(&im->src, hd + 4, 8) && hd[4] == 1;
        int w = hd[6] | hd[7] << 8, h = hd[8] | hd[9] << 8, n = hd[10] | hd[11] << 8;
        if (!ok || n < 1 || n > MINI_IMAGE_MAX_FRAMES || w < 1 || h < 1 || w > MINI_IMAGE_MAX_SIDE ||
            h > MINI_IMAGE_MAX_SIDE) {
            set_err(err, cap, "bad animation header");
            mini_image_close(im);
            return NULL;
        }
        im->frames = n;
        if (!frames_room(im, n)) {
            set_err(err, cap, "out of memory");
            mini_image_close(im);
            return NULL;
        }
        for (int i = 0; ok && i < n; i++) {
            uint8_t e[10];
            ok = src_read(&im->src, e, 10);
            im->off[i] = (uint32_t)e[0] | (uint32_t)e[1] << 8 | (uint32_t)e[2] << 16 | (uint32_t)e[3] << 24;
            im->delay[i] = (uint16_t)(e[8] | e[9] << 8);
        }
        if (!ok) {
            set_err(err, cap, "bad animation frame table");
            mini_image_close(im);
            return NULL;
        }
        im->w = w;
        im->h = h;
    } else {
        im->frames = 1;
        if (!frames_room(im, 1)) {
            set_err(err, cap, "out of memory");
            mini_image_close(im);
            return NULL;
        }
        im->off[0] = 0;
        im->delay[0] = 0;
    }
    if (!load_frame(im, 0, err, cap)) {
        mini_image_close(im);
        return NULL;
    }
    if (im->w > MINI_IMAGE_MAX_SIDE || im->h > MINI_IMAGE_MAX_SIDE) {
        set_err(err, cap, "image too large");
        mini_image_close(im);
        return NULL;
    }
    return im;
}

void mini_image_close(MiniImage *im) {
    if (!im) return;
    if (im->src.f) s_io.close(im->src.f);
    im->src.f = NULL;
    if (!s_spare) {
        s_spare = im;   /* kept for the next image */
        return;
    }
    free(im->off);
    free(im->delay);
    free(im->snaps);
    free(im);
}

int mini_image_width(const MiniImage *im) { return im ? im->w : 0; }
int mini_image_height(const MiniImage *im) { return im ? im->h : 0; }
int mini_image_frames(const MiniImage *im) { return im ? im->frames : 0; }

uint32_t mini_image_delay(const MiniImage *im, int frame) {
    if (!im || frame < 0 || frame >= im->frames || im->frames < 2) return 0;
    return im->delay[frame];
}

int mini_image_draw(MiniImage *im, int frame, int x0, int x1, int y0, int y1,
                    MiniImageRowFn fn, void *ud, char *err, size_t cap) {
    if (!im || !fn || frame < 0 || frame >= im->frames) {
        set_err(err, cap, "no such frame");
        return -1;
    }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > im->w) x1 = im->w;
    if (y1 > im->h) y1 = im->h;
    if (x0 >= x1 || y0 >= y1) return 0;
    if (!load_frame(im, frame, err, cap)) return -1;
    int r0 = y0 / im->mcu_h, r1 = (y1 - 1) / im->mcu_h;
    for (int r = r0; r <= r1; r++) {
        if (r != im->next_row) {
            /* the latest known row start at or before r, then forward */
            int from = r < im->known ? r : im->known;
            if (!snap_restore(im, &im->snaps[from])) {
                set_err(err, cap, "cannot seek in the image file");
                return -1;
            }
            im->next_row = from;
            while (im->next_row < r) {
                if (!decode_row(im, im->next_row, 0, 0, 0, 0, NULL, NULL)) {
                    set_err(err, cap, "corrupt JPEG data");
                    im->frame = -1;
                    return -1;
                }
                im->next_row++;
                if (im->next_row > im->known) {
                    im->known = im->next_row;
                    snap_take(im, &im->snaps[im->known]);
                }
            }
        }
        if (!decode_row(im, r, x0, x1, y0, y1, fn, ud)) {
            set_err(err, cap, "corrupt JPEG data");
            im->frame = -1;
            return -1;
        }
        im->next_row = r + 1;
        if (im->next_row > im->known && im->next_row <= im->mcus_y) {
            im->known = im->next_row;
            snap_take(im, &im->snaps[im->known]);
        }
    }
    return 0;
}
