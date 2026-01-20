#ifndef UTILS_H
#define UTILS_H

#include "esp_err.h"

// Fetch image from URL, convert to BMP, and save to Downloads album
// Returns ESP_OK on success, error code on failure
// saved_bmp_path will contain the path to the saved BMP file
esp_err_t fetch_and_save_image_from_url(const char *url, char *saved_bmp_path, size_t path_size);

// Trigger image rotation based on configured rotation mode
// Handles both URL and SD card rotation modes
// Returns ESP_OK on success, error code on failure
esp_err_t trigger_image_rotation(void);

// Create battery status JSON object with all battery fields
// Returns cJSON object (caller must delete with cJSON_Delete)
// Returns NULL on failure
struct cJSON;
struct cJSON *create_battery_json(void);

// Calculate next clock-aligned wake-up time considering sleep schedule
// Returns seconds until next wake-up
// Takes into account:
// - Clock alignment (aligns to rotation interval boundaries)
// - Sleep schedule (skips wake-ups that fall within sleep schedule)
// - Overnight schedules (handles schedules that cross midnight)
int calculate_next_aligned_wakeup(int rotate_interval);

#endif
