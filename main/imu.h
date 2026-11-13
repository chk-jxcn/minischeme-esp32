/**
 * @file imu.h
 * @brief The `imu` API extension (imu.c): the QMI8658 accelerometer / gyroscope, only in builds
 *        for a board with one (MINI_BOARD=matrix).
 */

#ifndef IMU_H
#define IMU_H

#include <stdbool.h>

#include "minischeme.h"

/** @brief Find and start the sensor. @return false when there is none. */
bool imu_init(void);
/** @brief Whether imu_init() found the sensor. */
bool imu_ready(void);
/** @brief The `imu-...` procedures into a VM. */
void imu_register(MiniVM *vm);

#endif
