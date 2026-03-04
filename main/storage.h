#pragma once

#include <esp_err.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Storage backend types
typedef enum {
    STORAGE_TYPE_NONE = 0,
    STORAGE_TYPE_SDCARD,
    STORAGE_TYPE_LITTLEFS,
    STORAGE_TYPE_MEMFS
} storage_type_t;

/**
 * @brief Initialize the appropriate storage medium based on kconfig and availability
 *        Handles fallbacks (SD Card -> LittleFS -> MemFS)
 *
 * @return esp_err_t ESP_OK on success, ESP_FAIL or error code on failure
 */
esp_err_t storage_init(void);

/**
 * @brief Get the currently active primary storage type
 *
 * @return storage_type_t The active storage type
 */
storage_type_t storage_get_type(void);

/**
 * @brief Check if SD card is present and mounted
 *
 * @return true if SD card is available, false otherwise
 */
bool storage_has_sdcard(void);

/**
 * @brief Check if any persistent storage (SD or Flash) is available
 *
 * @return true if persistent storage is available, false otherwise
 */
bool storage_has_persistent_storage(void);

/**
 * @brief Check if the system can process images to files
 *        Currently, this is only practical on systems with an SD card
 *        due to internal flash fragmentation/wear issues.
 *
 * @return true if processing to file is supported
 */
bool storage_can_process_to_file(void);

/**
 * @brief Read WiFi credentials from "wifi.txt" file on root storage (if available)
 *
 * @param ssid Buffer to store SSID (must be at least WIFI_SSID_MAX_LEN)
 * @param password Buffer to store password (must be at least WIFI_PASS_MAX_LEN)
 * @return esp_err_t ESP_OK if found and read successfully, ESP_ERR_NOT_FOUND if not found
 */
esp_err_t storage_read_wifi_credentials(char *ssid, char *password);

#ifdef __cplusplus
}
#endif
