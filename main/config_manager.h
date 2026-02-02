#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <stdbool.h>

#include "config.h"
#include "esp_err.h"

esp_err_t config_manager_init(void);

// ============================================================================
// General
// ============================================================================

void config_manager_set_device_name(const char *name);
const char *config_manager_get_device_name(void);

void config_manager_set_timezone(const char *tz);
const char *config_manager_get_timezone(void);

void config_manager_set_display_orientation(display_orientation_t orientation);
display_orientation_t config_manager_get_display_orientation(void);

void config_manager_set_display_rotation_deg(int rotation_deg);
int config_manager_get_display_rotation_deg(void);

// ============================================================================
// Auto Rotate
// ============================================================================

void config_manager_set_auto_rotate(bool enabled);
bool config_manager_get_auto_rotate(void);

void config_manager_set_rotate_interval(int seconds);
int config_manager_get_rotate_interval(void);

void config_manager_set_auto_rotate_aligned(bool enabled);
bool config_manager_get_auto_rotate_aligned(void);

void config_manager_set_sleep_schedule_enabled(bool enabled);
bool config_manager_get_sleep_schedule_enabled(void);

void config_manager_set_sleep_schedule_start(int minutes);
int config_manager_get_sleep_schedule_start(void);

void config_manager_set_sleep_schedule_end(int minutes);
int config_manager_get_sleep_schedule_end(void);

bool config_manager_is_in_sleep_schedule(void);

void config_manager_set_rotation_mode(rotation_mode_t mode);
rotation_mode_t config_manager_get_rotation_mode(void);

// ============================================================================
// Auto Rotate - SDCARD
// ============================================================================

void config_manager_set_sd_rotation_mode(sd_rotation_mode_t mode);
sd_rotation_mode_t config_manager_get_sd_rotation_mode(void);

void config_manager_set_last_index(int32_t index);
int32_t config_manager_get_last_index(void);

// ============================================================================
// Auto Rotate - URL
// ============================================================================

void config_manager_set_image_url(const char *url);
const char *config_manager_get_image_url(void);

void config_manager_set_access_token(const char *token);
const char *config_manager_get_access_token(void);

void config_manager_set_http_header_key(const char *key);
const char *config_manager_get_http_header_key(void);

void config_manager_set_http_header_value(const char *value);
const char *config_manager_get_http_header_value(void);

void config_manager_set_save_downloaded_images(bool enabled);
bool config_manager_get_save_downloaded_images(void);

// ============================================================================
// Home Assistant
// ============================================================================

void config_manager_set_ha_url(const char *url);
const char *config_manager_get_ha_url(void);

#endif
