#ifndef PCF8563_RTC_H
#define PCF8563_RTC_H

#include <stdbool.h>
#include <time.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize PCF8563 RTC
 *
 * @param i2c_bus I2C bus handle from board HAL
 * @return ESP_OK on success, error code on failure
 */
esp_err_t pcf8563_init(i2c_master_bus_handle_t i2c_bus);

/**
 * @brief Read time from PCF8563 RTC
 *
 * The registers are read as UTC (the way pcf8563_write_time() stores them).
 *
 * @param time_out Pointer to store the time
 * @return ESP_OK on success, error code on failure
 */
esp_err_t pcf8563_read_time(time_t *time_out);

/**
 * @brief Write time to PCF8563 RTC
 *
 * The time is stored as UTC so a DST changeover cannot shift what reads back.
 *
 * @param time_in Time to write
 * @return ESP_OK on success, error code on failure
 */
esp_err_t pcf8563_write_time(time_t time_in);

/**
 * @brief Check if PCF8563 RTC is available
 *
 * @return true if RTC is available, false otherwise
 */
bool pcf8563_is_available(void);

#ifdef __cplusplus
}
#endif

#endif  // PCF8563_RTC_H
