#pragma once

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(CONFIG_SDCARD_DRIVER_SDIO)
/**
 * @brief SD card configuration for SDIO interface
 */
typedef struct {
    gpio_num_t clk_pin;
    gpio_num_t cmd_pin;
    gpio_num_t d0_pin;
    gpio_num_t d1_pin;
    gpio_num_t d2_pin;
    gpio_num_t d3_pin;
} sdcard_config_t;

#elif defined(CONFIG_SDCARD_DRIVER_SPI)
/**
 * @brief SD card configuration for SPI interface
 */
typedef struct {
    int host_id;
    gpio_num_t cs_pin;
} sdcard_config_t;

#elif defined(CONFIG_SDCARD_DRIVER_INTERNAL_FLASH)
/**
 * @brief Configuration for internal flash storage
 */
typedef struct {
    int dummy; // No specific pins needed, but struct must exist
} sdcard_config_t;

#else
/**
 * @brief Fallback empty config if no SD card driver is selected
 */
typedef struct {
    int dummy;
} sdcard_config_t;
#endif

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

#ifdef __cplusplus
}
#endif