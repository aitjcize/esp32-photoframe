#ifndef BLE_WAKE_SERVICE_H
#define BLE_WAKE_SERVICE_H

#include <stdbool.h>
#include "esp_err.h"

// Initialize BLE wake service
esp_err_t ble_wake_service_init(void);

// Start BLE advertising
esp_err_t ble_wake_service_start(void);

// Stop BLE advertising
esp_err_t ble_wake_service_stop(void);

// Check if BLE wake service is running
bool ble_wake_service_is_running(void);

// Enable/disable BLE wake mode
esp_err_t ble_wake_service_set_enabled(bool enabled);

// Get BLE wake mode status
bool ble_wake_service_get_enabled(void);

#endif
