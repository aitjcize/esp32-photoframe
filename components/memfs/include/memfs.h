#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize and mount a RAM-based virtual filesystem
 *
 * @param base_path The mount point (e.g., "/storage")
 * @param max_files Maximum number of files to support
 * @return esp_err_t ESP_OK on success
 */
esp_err_t memfs_mount(const char *base_path, size_t max_files);

/**
 * @brief Unmount and free all memory used by the RAM filesystem
 *
 * @param base_path The mount point used during registration
 * @return esp_err_t ESP_OK on success
 */
esp_err_t memfs_unmount(const char *base_path);

/**
 * @brief Get total bytes used by files in the RAM filesystem
 *
 * @return size_t Total bytes
 */
size_t memfs_get_total_used(void);

#ifdef __cplusplus
}
#endif
