/**
 * @file minischeme_image.h
 * @brief Images from files: baseline JPEG, and animations (a GIF converted
 *        by the album's web page) made of JPEG frames.
 *
 * The board has no frame buffer: the screen is drawn band by band (13 rows)
 * and every band asks only for its rows. A JPEG is decoded in MCU rows (8
 * or 16 pixel rows); the decoder remembers where each MCU row starts in the
 * entropy-coded data (its bit position and DC predictors), so a band
 * decodes just the MCU rows it covers. Memory: about 5 KB while an image is
 * open (Huffman and quantization tables, one MCU), plus 12 bytes per MCU
 * row; nothing per pixel.
 *
 * PNG, GIF, WebP and progressive JPEG are converted in the browser (the
 * album's web page) into baseline JPEG: inflating a PNG needs a 32 KB
 * window and a GIF's frames need a full-screen canvas to be composed.
 *
 * Animation file ("MANI", little-endian):
 *   "MANI" | u8 version 1 | u8 flags | u16 width | u16 height | u16 frames
 *   then per frame: u32 offset | u32 length | u16 delay ms
 *   then the frames: each a baseline JPEG of width x height
 */

#ifndef MINISCHEME_IMAGE_H
#define MINISCHEME_IMAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MINI_IMAGE_MAX_FRAMES 512
#define MINI_IMAGE_MAX_SIDE   4096

/** How images are read: stdio by default; a host without one sets its own. */
typedef struct {
    void *(*open)(const char *path);
    int (*read)(void *f, void *buf, int n);      /* bytes read, 0 at the end, < 0 an error */
    bool (*seek)(void *f, long offset);          /* from the start */
    void (*close)(void *f);
} MiniImageIO;
void mini_image_set_io(const MiniImageIO *io);

typedef struct MiniImage MiniImage;

/** @brief Opens path (a .jpg or a MANI animation); NULL with err set. */
MiniImage *mini_image_open(const char *path, char *err, size_t err_cap);
void mini_image_close(MiniImage *im);
int mini_image_width(const MiniImage *im);
int mini_image_height(const MiniImage *im);
int mini_image_frames(const MiniImage *im);
/** @brief Frame delay in ms (0 for a still image). */
uint32_t mini_image_delay(const MiniImage *im, int frame);

/** Gets a run of decoded pixels: n RGB565 pixels at (x, y) of the image */
typedef void (*MiniImageRowFn)(void *ud, int x, int y, const uint16_t *px, int n);
/** @brief Decodes the pixels of frame in columns [x0, x1) and rows [y0, y1)
 *         (image coordinates) and hands them to fn; 0, or -1 with err set. */
int mini_image_draw(MiniImage *im, int frame, int x0, int x1, int y0, int y1,
                    MiniImageRowFn fn, void *ud, char *err, size_t err_cap);

#endif /* MINISCHEME_IMAGE_H */
