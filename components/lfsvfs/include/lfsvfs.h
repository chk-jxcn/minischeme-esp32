/**
 * @file lfsvfs.h
 * @brief LittleFS on an ESP-IDF data partition, mounted into the VFS so the
 *        C library (fopen, opendir, rename, ...) works on it.
 *
 * Paths behave like SPIFFS for the rest of the firmware: creating a file
 * creates its missing parent directories, so "/spiffs/fonts/x.mfnt" can be
 * written without a mkdir. littlefs itself (littlefs/, BSD-3-Clause) is
 * unmodified.
 */

#ifndef LFSVFS_H
#define LFSVFS_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/**
 * @brief Mounts the partition at base_path.
 * @param format_if_failed format when it holds no LittleFS
 * @param max_files open files at the same time
 */
esp_err_t lfsvfs_mount(const char *base_path, const char *partition_label, bool format_if_failed, int max_files);
/** @brief Erases the partition into an empty LittleFS (not mounted). */
esp_err_t lfsvfs_format(const char *partition_label);
void lfsvfs_unmount(void);
bool lfsvfs_mounted(void);
/** @brief Partition size and bytes in use. */
esp_err_t lfsvfs_info(size_t *total, size_t *used);

#endif /* LFSVFS_H */
