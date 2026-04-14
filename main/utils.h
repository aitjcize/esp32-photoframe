#ifndef UTILS_H
#define UTILS_H

#include <stdbool.h>

#include "cJSON.h"
#include "esp_err.h"

// Apply config values from a parsed cJSON object.
// Handles all config fields including side effects (WiFi, mDNS, timers, etc.).
// Fields not present in the JSON are left unchanged.
// Returns ESP_FAIL if WiFi connection fails (other fields still applied up to that point).
// Does NOT call config_manager_touch_config() - caller is responsible.
// Does NOT take ownership of root - caller must free with cJSON_Delete().
esp_err_t apply_config_from_json(cJSON *root);

// Get/set the last image fetch error (transient, for UI display)
void utils_set_last_fetch_error(const char *error);
const char *utils_get_last_fetch_error(void);

// Last cert pin error (transient). set: stash a message after a failed pin.
// consume: read and clear; returns "" if empty. Used by the config HTTP handler
// to surface why apply_config_from_json returned ESP_FAIL.
void utils_set_cert_pin_error(const char *msg);
const char *utils_consume_cert_pin_error(void);

// Fetch image from URL, process it, and save to Downloads album.
// Returns ESP_OK on success (including 304), error code on failure.
// On success with a downloaded image, saved_image_path will contain the path
// to the processed image (PNG). On HTTP 304 Not Modified, *not_modified (if
// non-NULL) is set to true and saved_image_path is left as an empty string —
// the caller should skip display refresh because the eInk already holds the
// correct image. not_modified may be NULL if the caller doesn't care.
esp_err_t fetch_and_save_image_from_url(const char *url, char *saved_image_path, size_t path_size,
                                        bool *not_modified);

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
