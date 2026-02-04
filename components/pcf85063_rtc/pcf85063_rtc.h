#ifndef PCF85063_RTC_H
#define PCF85063_RTC_H

#include <stdbool.h>
#include <time.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize PCF85063 RTC
 *
 * @param i2c_bus I2C bus handle from board HAL
 * @return ESP_OK on success, error code on failure
 */
esp_err_t pcf85063_init(i2c_master_bus_handle_t i2c_bus);

/**
 * @brief Read time from PCF85063 RTC
 *
 * @param time_out Pointer to store the time
 * @return ESP_OK on success, error code on failure
 */
esp_err_t pcf85063_read_time(time_t *time_out);

/**
 * @brief Write time to PCF85063 RTC
 *
 * @param time_in Time to write
 * @return ESP_OK on success, error code on failure
 */
esp_err_t pcf85063_write_time(time_t time_in);

/**
 * @brief Check if PCF85063 RTC is available
 *
 * @return true if RTC is available, false otherwise
 */
bool pcf85063_is_available(void);

#ifdef __cplusplus
}
#endif

#endif  // PCF85063_RTC_H
