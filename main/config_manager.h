#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

void config_manager_set_ntp_server(const char *server);
const char *config_manager_get_ntp_server(void);

void config_manager_set_display_orientation(display_orientation_t orientation);
display_orientation_t config_manager_get_display_orientation(void);

void config_manager_set_display_rotation_deg(int rotation_deg);
int config_manager_get_display_rotation_deg(void);

void config_manager_set_wifi_ssid(const char *ssid);
const char *config_manager_get_wifi_ssid(void);

void config_manager_set_wifi_password(const char *password);
const char *config_manager_get_wifi_password(void);

void config_manager_set_wifi_mode(wifi_mode_setting_t mode);
wifi_mode_setting_t config_manager_get_wifi_mode(void);

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

void config_manager_set_ca_cert_der(const uint8_t *der, size_t len);
const uint8_t *config_manager_get_ca_cert_der(size_t *out_len);

void config_manager_set_access_token(const char *token);
const char *config_manager_get_access_token(void);

void config_manager_set_http_header_key(const char *key);
const char *config_manager_get_http_header_key(void);

void config_manager_set_http_header_value(const char *value);
const char *config_manager_get_http_header_value(void);

void config_manager_set_save_downloaded_images(bool enabled);
bool config_manager_get_save_downloaded_images(void);

// ETag captured from the last successful image fetch.
// Empty string means "not set" — skip the If-None-Match request header.
void config_manager_set_image_etag(const char *etag);
const char *config_manager_get_image_etag(void);

// ============================================================================
// Home Assistant
// ============================================================================

void config_manager_set_ha_url(const char *url);
const char *config_manager_get_ha_url(void);

// ============================================================================
// AI API Keys
// ============================================================================

void config_manager_set_openai_api_key(const char *key);
const char *config_manager_get_openai_api_key(void);

void config_manager_set_google_api_key(const char *key);
const char *config_manager_get_google_api_key(void);

// ============================================================================
// Power
// ============================================================================

void config_manager_set_deep_sleep_enabled(bool enabled);
bool config_manager_get_deep_sleep_enabled(void);

// ============================================================================
// Config Sync
// ============================================================================

void config_manager_set_config_last_updated(int64_t timestamp);
int64_t config_manager_get_config_last_updated(void);
void config_manager_touch_config(void);  // Set last_updated to current time

#endif
