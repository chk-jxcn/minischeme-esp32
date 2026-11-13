/**
 * @file minischeme_matrix.h
 * @brief An RGB LED matrix as a board peripheral (the Waveshare ESP32-S3-Matrix: 8 x 8
 *        WS2812). Pixels are kept in a buffer and sent with mini_matrix_show().
 *
 * Coordinates: x 0..W-1 from the left, y 0..H-1 from the top, as the board is configured
 * (mini_matrix_configure). Colours are 0xRRGGBB.
 */

#ifndef MINISCHEME_MATRIX_H
#define MINISCHEME_MATRIX_H

#include <stdbool.h>
#include <stdint.h>

#define MINI_MATRIX_W 8
#define MINI_MATRIX_H 8

/** @brief Highest value a colour channel is ever driven with (of 255): 64 LEDs at full
 *  white draw several amperes and heat the board within seconds. */
#define MINI_MATRIX_MAX_LEVEL 80

/** @brief Sets up the output; false when the board has no matrix or the driver failed. */
bool mini_matrix_init(void);

/** @brief The board has a matrix and it is ready. */
bool mini_matrix_ready(void);

/** @brief How the LEDs are wired: rgb_order (else GRB), serpentine (every other row runs
 *  backwards, else all rows run the same way), rotate (quarter turns clockwise, 0..3). */
void mini_matrix_configure(bool rgb_order, bool serpentine, int rotate);

void mini_matrix_set(int x, int y, uint32_t rgb);
uint32_t mini_matrix_get(int x, int y);
void mini_matrix_fill(uint32_t rgb);

/** @brief Brightness in percent (0..100) of MINI_MATRIX_MAX_LEVEL; applied when shown. */
void mini_matrix_set_brightness(int percent);
int mini_matrix_get_brightness(void);

/** @brief Sends the buffer to the LEDs (about 2.5 ms). */
bool mini_matrix_show(void);

#endif
