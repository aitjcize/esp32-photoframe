#ifndef HA_INTEGRATION_H
#define HA_INTEGRATION_H

#include <stdbool.h>

#include "esp_err.h"

/**
 * @brief Check if Home Assistant URL is configured
 *
 * @return true if HA URL is configured, false otherwise
 */
bool ha_is_configured(void);

/**
 * @brief Notify Home Assistant that device is online (triggers HA to poll all data via REST API)
 *
 * The response may carry a rotation decision computed by the HA integration.
 *
 * @param out_should_rotate If non-NULL, set to whether this wake should rotate
 *        the image (defaults to true; set to false when HA vetoes the rotation).
 *        Only meaningful when the call returns ESP_OK.
 * @return ESP_OK on success, ESP_FAIL / esp_err_t on error
 */
esp_err_t ha_notify_online(bool *out_should_rotate);

/**
 * @brief Notify Home Assistant that device is going offline (entering deep sleep)
 *
 * @return ESP_OK on success, ESP_FAIL on error
 */
esp_err_t ha_notify_offline(void);

/**
 * @brief Notify Home Assistant that device has new data available (e.g., after OTA check)
 *
 * @return ESP_OK on success, ESP_FAIL on error
 */
esp_err_t ha_notify_update(void);

#endif
