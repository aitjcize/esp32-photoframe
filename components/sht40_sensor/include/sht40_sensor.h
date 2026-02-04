#pragma once

#include <stdbool.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize SHT40 temperature and humidity sensor
 *
 * @param i2c_bus I2C bus handle from board HAL
 * @return ESP_OK on success, error code on failure
 */
esp_err_t sht40_init(i2c_master_bus_handle_t i2c_bus);

/**
 * @brief Read temperature and humidity from SHT40 sensor
 *
 * @param temperature Pointer to store temperature in Celsius
 * @param humidity Pointer to store relative humidity in percent
 * @return ESP_OK on success, error code on failure
 */
esp_err_t sht40_read(float *temperature, float *humidity);

/**
 * @brief Check if SHT40 sensor is available and responding
 *
 * @return true if sensor is available, false otherwise
 */
bool sht40_is_available(void);

#ifdef __cplusplus
}
#endif
