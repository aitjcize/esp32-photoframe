#ifndef UTILS_H
#define UTILS_H

#include "esp_err.h"

// Fetch image from URL, process it, and save to Downloads album
// Returns ESP_OK on success, error code on failure
// saved_image_path will contain the path to the processed image (PNG)
esp_err_t fetch_and_save_image_from_url(const char *url, char *saved_image_path, size_t path_size);

// Trigger image rotation based on configured rotation mode
// Handles both URL and SD card rotation modes
// Returns ESP_OK on success, error code on failure
esp_err_t trigger_image_rotation(void);

// Create battery status JSON object with all battery fields
// Returns cJSON object (caller must delete with cJSON_Delete)
// Returns NULL on failure
struct cJSON;
struct cJSON *create_battery_json(void);

// Calculate next wake-up time considering sleep schedule
// Returns seconds until next wake-up
// Takes into account:
// - Clock alignment (aligns to rotation interval boundaries if enabled)
// - Sleep schedule (skips wake-ups that fall within sleep schedule)
// - Overnight schedules (handles schedules that cross midnight)
int get_seconds_until_next_wakeup(void);

// Sanitize device name to create a valid mDNS hostname
// Converts to lowercase, replaces spaces and special chars with hyphens
// Example: "Living Room PhotoFrame" -> "living-room-photoframe"
// hostname buffer must be at least max_len bytes
void sanitize_hostname(const char *device_name, char *hostname, size_t max_len);

// Get unique device ID (MAC address in hex)
// Returns pointer to static buffer containing ID
// Buffer is at least 13 bytes (12 chars + null)
const char *get_device_id(void);

#endif
