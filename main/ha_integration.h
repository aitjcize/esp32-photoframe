#ifndef HA_INTEGRATION_H
#define HA_INTEGRATION_H

#include <stdbool.h>

#include "esp_err.h"

/**
 * @brief Initialize HA integration (starts battery push task)
 *
 * @return ESP_OK on success, ESP_FAIL on error
 */
esp_err_t ha_integration_init(void);

/**
 * @brief Post battery info to Home Assistant
 *
 * @return ESP_OK on success, ESP_FAIL on error
 */
esp_err_t ha_post_battery_info(void);

/**
 * @brief Check if Home Assistant URL is configured
 *
 * @return true if HA URL is configured, false otherwise
 */
bool ha_is_configured(void);

/**
 * @brief Trigger OTA firmware update (for HA integration)
 *
 * @return ESP_OK on success, ESP_FAIL on error
 */
esp_err_t ha_trigger_ota_update(void);

#endif
