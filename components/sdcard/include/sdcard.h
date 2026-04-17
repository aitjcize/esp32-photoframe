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
    gpio_num_t power_en_pin;  // GPIO for power enable (e.g. TPS22916), -1 if not used
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
 * @brief Deinitialize SD card and cut power if power_en_pin is configured
 */
void sdcard_deinit(void);

#ifdef __cplusplus
}
#endif