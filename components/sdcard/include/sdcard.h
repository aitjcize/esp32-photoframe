#pragma once

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief SD card configuration
 */
typedef struct {
    const char *mount_point;
#ifdef CONFIG_SDCARD_DRIVER_SPI
    int host_id;
    gpio_num_t cs_pin;
#endif
#ifdef CONFIG_SDCARD_DRIVER_SDIO
    gpio_num_t clk_pin;
    gpio_num_t cmd_pin;
    gpio_num_t d0_pin;
    gpio_num_t d1_pin;
    gpio_num_t d2_pin;
    gpio_num_t d3_pin;
#endif
} sdcard_config_t;

/**
 * @brief Initialize SD card
 *
 * @param config Pointer to configuration structure
 * @return esp_err_t ESP_OK on success, error code otherwise
 */
esp_err_t sdcard_init(const sdcard_config_t *config);

/**
 * @brief Check if SD card is mounted
 *
 * @return true if mounted, false otherwise
 */
bool sdcard_is_mounted(void);

/**
 * @brief Unmount the SD card and release host resources
 *
 * Safe to call when no card is mounted (returns ESP_OK). Use before cutting
 * SD card power so the bus is released cleanly.
 *
 * @return esp_err_t ESP_OK on success or when nothing was mounted
 */
esp_err_t sdcard_deinit(void);

/**
 * @brief Format the mounted SD card as FAT32 (erases all data).
 *
 * Requires the card to already be mounted via sdcard_init(). The card
 * remains mounted at the same mount point after a successful format.
 *
 * @return esp_err_t ESP_OK on success, ESP_ERR_INVALID_STATE if no card
 *         is mounted, or the underlying FATFS error code.
 */
esp_err_t sdcard_format(void);

#ifdef __cplusplus
}
#endif