#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BOARD_TYPE_WAVESHARE_PHOTOPAINTER,
    BOARD_TYPE_SEEEDSTUDIO_XIAO_EE02,
    BOARD_TYPE_UNKNOWN
} board_type_t;

/**
 * @brief Get the board type
 *
 * @return board_type_t The current board type
 */
board_type_t board_hal_get_type(void);

/**
 * @brief Get the board name
 *
 * @return const char* The internal name of the board (used for OTA binaries, etc.)
 */
const char *board_hal_get_name(void);

/**
 * @brief Initialize the power management HAL
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t board_hal_init(void);

/**
 * @brief Prepare the system for deep sleep
 *
 * This function should be called just before esp_deep_sleep_start().
 * It handles PMIC-specific shutdown sequences (e.g. disabling rails).
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t board_hal_prepare_for_sleep(void);

/**
 * @brief Is battery connected
 *
 * @return true if connected, false otherwise
 */
bool board_hal_is_battery_connected(void);

/**
 * @brief Get battery percentage
 *
 * @return int Battery percentage (0-100), or -1 if unknown
 */
int board_hal_get_battery_percent(void);

/**
 * @brief Get battery voltage in millivolts
 *
 * @return int Battery voltage in mV, or -1 if unknown
 */
int board_hal_get_battery_voltage(void);

/**
 * @brief Check if battery is currently charging
 *
 * @return true if charging, false otherwise
 */
bool board_hal_is_charging(void);

/**
 * @brief Check if USB power is connected
 *
 * @return true if USB connected, false otherwise
 */
bool board_hal_is_usb_connected(void);

/**
 * @brief Perform a hard shutdown (power off)
 *
 * Note: Behavior depends on hardware. Some PMICs can cut power completely.
 */
void board_hal_shutdown(void);

/**
 * @brief Get ambient temperature (if sensor available)
 *
 * @param[out] t Pointer to float to store temperature in Celsius
 * @return esp_err_t ESP_OK on success, ESP_ERR_NOT_SUPPORTED if no sensor
 */
esp_err_t board_hal_get_temperature(float *t);

/**
 * @brief Get ambient humidity (if sensor available)
 *
 * @param[out] h Pointer to float to store humidity in %RH
 * @return esp_err_t ESP_OK on success, ESP_ERR_NOT_SUPPORTED if no sensor
 */
esp_err_t board_hal_get_humidity(float *h);

/**
 * @brief Initialize the external RTC (if available)
 *
 * @return esp_err_t ESP_OK on success, ESP_ERR_NOT_SUPPORTED if no RTC, or other error
 */
esp_err_t board_hal_rtc_init(void);

/**
 * @brief Get time from external RTC
 *
 * @param[out] t Time value to populate
 * @return esp_err_t ESP_OK on success
 */
esp_err_t board_hal_rtc_get_time(time_t *t);

/**
 * @brief Set time to external RTC
 *
 * @param t Time value to set
 * @return esp_err_t ESP_OK on success
 */
esp_err_t board_hal_rtc_set_time(time_t t);

/**
 * @brief Get display width
 *
 * @return uint16_t Width in pixels
 */
uint16_t board_hal_get_display_width(void);

/**
 * @brief Get display height
 *
 * @return uint16_t Height in pixels
 */
uint16_t board_hal_get_display_height(void);

/**
 * @brief Get default display rotation
 *
 * @return uint16_t Rotation in degrees (0, 90, 180, 270)
 */
uint16_t board_hal_get_display_rotation(void);

/**
 * @brief Check if external RTC is available/initialized
 *
 * @return true if available
 */
bool board_hal_rtc_is_available(void);

#ifdef __cplusplus
}
#endif
