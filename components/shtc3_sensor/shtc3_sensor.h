#ifndef SHTC3_SENSOR_H
#define SHTC3_SENSOR_H

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize SHTC3 temperature and humidity sensor
 *
 * @return ESP_OK on success, error code on failure
 */
esp_err_t shtc3_init(void);

/**
 * @brief Read temperature and humidity from SHTC3 sensor
 *
 * @param temperature Pointer to store temperature in Celsius
 * @param humidity Pointer to store relative humidity in percent
 * @return ESP_OK on success, error code on failure
 */
esp_err_t shtc3_read(float *temperature, float *humidity);

/**
 * @brief Check if SHTC3 sensor is available and responding
 *
 * @return true if sensor is available, false otherwise
 */
bool shtc3_is_available(void);

/**
 * @brief Put SHTC3 sensor into sleep mode to save power
 *
 * @return ESP_OK on success, error code on failure
 */
esp_err_t shtc3_sleep(void);

/**
 * @brief Wake up SHTC3 sensor from sleep mode
 *
 * @return ESP_OK on success, error code on failure
 */
esp_err_t shtc3_wakeup(void);

#ifdef __cplusplus
}
#endif

#endif  // SHTC3_SENSOR_H
