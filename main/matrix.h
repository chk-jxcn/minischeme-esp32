/**
 * @file matrix.h
 * @brief The `matrix` API extension (matrix.c): only in builds for a board with an RGB LED
 *        matrix (MINI_BOARD=matrix).
 */

#ifndef MATRIX_H
#define MATRIX_H

#include "minischeme.h"

/** @brief The `matrix-...` procedures into a VM. */
void matrix_register(MiniVM *vm);

#endif
