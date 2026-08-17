#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "cron.h"
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

// Advanced network settings (#43), same collapsed UI section as the NTP server
// above: static IP (instead of DHCP) and DNS override. The DNS override applies
// in both IP modes (empty string = automatic). Values are dotted IPv4 strings.
void config_manager_set_ip_mode(ip_mode_t mode);
ip_mode_t config_manager_get_ip_mode(void);
void config_manager_set_static_ip(const char *ip);
const char *config_manager_get_static_ip(void);
void config_manager_set_static_netmask(const char *mask);
const char *config_manager_get_static_netmask(void);
void config_manager_set_static_gateway(const char *gw);
const char *config_manager_get_static_gateway(void);
void config_manager_set_dns_server(const char *dns);
const char *config_manager_get_dns_server(void);

void config_manager_set_display_orientation(display_orientation_t orientation);
display_orientation_t config_manager_get_display_orientation(void);

void config_manager_set_display_rotation_deg(int rotation_deg);
int config_manager_get_display_rotation_deg(void);

void config_manager_set_wifi_ssid(const char *ssid);
const char *config_manager_get_wifi_ssid(void);

void config_manager_set_wifi_password(const char *password);
const char *config_manager_get_wifi_password(void);

// ============================================================================
// Auto Rotate
// ============================================================================

void config_manager_set_auto_rotate(bool enabled);
bool config_manager_get_auto_rotate(void);

int config_manager_get_cron_rule_count(void);

// Returns NULL if index is out of range.
const char *config_manager_get_cron_rule(int index);

// Replaces the rule set (caps at MAX_CRON_RULES, drops empty/over-long entries);
// caller should validate expressions with cron_parse() first.
void config_manager_set_cron_rules(const char *const *rules, int count);
void config_manager_set_cron_rules_from_interval(int seconds);

// Compiles the stored rules into `out`; returns the count written (<= max),
// skipping any that fail to parse.
int config_manager_get_compiled_cron_rules(cron_rule_t *out, int max);

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

// Master switch for all Home Assistant integration (online/offline/update
// notifications and the rotation-veto piggyback). Defaults to false on a
// fresh device; a device upgrading from a firmware version that predates
// this switch keeps HA enabled automatically if a ha_url was already
// configured, so existing setups don't silently break.
void config_manager_set_ha_enabled(bool enabled);
bool config_manager_get_ha_enabled(void);

// ============================================================================
// Telegram Bot
// ============================================================================

void config_manager_set_telegram_bot_token(const char *token);
const char *config_manager_get_telegram_bot_token(void);

void config_manager_set_telegram_chat_id(const char *chat_id);
const char *config_manager_get_telegram_chat_id(void);

// True once both a bot token and a chat ID are configured.
bool config_manager_telegram_is_configured(void);

// Highest Telegram update_id processed so far (0 = none yet). The next
// getUpdates poll should request offset = value + 1.
void config_manager_set_telegram_last_update_id(int64_t update_id);
int64_t config_manager_get_telegram_last_update_id(void);

// Orientation-pairing mode: combine two mismatched-orientation Telegram
// photos into one composed image instead of ever showing one alone.
// Togglable via the web UI and the "/pairing" bot command.
void config_manager_set_telegram_pairing_enabled(bool enabled);
bool config_manager_get_telegram_pairing_enabled(void);

// Queue of images waiting for an orientation partner (FIFO - oldest first).
// Every mismatched image is appended here, not just one, so nothing is lost
// if several arrive before a partner shows up. Persisted so it survives deep
// sleep even on MemFS-only boards where the files themselves won't (callers
// should stat() before trusting a path is still there).
void config_manager_add_telegram_pending_image(const char *path, const char *caption);
int config_manager_get_telegram_pending_image_count(void);
bool config_manager_get_telegram_pending_image_at(int index, char *path_out, size_t path_out_len,
                                                  char *caption_out, size_t caption_out_len);
void config_manager_remove_telegram_pending_image_at(int index);
// Clears the tracking queue only - does NOT delete the underlying files
// (they remain on storage like any other received image).
void config_manager_clear_telegram_pending_images(void);

// Whether a low-battery Telegram warning has already been sent for the
// current discharge episode (cleared once the battery recovers), so the
// warning fires once rather than on every poll.
void config_manager_set_telegram_low_battery_warned(bool warned);
bool config_manager_get_telegram_low_battery_warned(void);

// Wake-up status ping (SSID/IP/battery/wake reason/rotation schedule) sent to
// Telegram on every poll, even when there are no new updates.
void config_manager_set_telegram_wake_notify_enabled(bool enabled);
bool config_manager_get_telegram_wake_notify_enabled(void);

// ============================================================================
// AI API Keys
// ============================================================================

void config_manager_set_openai_api_key(const char *key);
const char *config_manager_get_openai_api_key(void);

void config_manager_set_google_api_key(const char *key);
const char *config_manager_get_google_api_key(void);

// ============================================================================
// Error overlay / WiFi failure tracking
// ============================================================================

// On-display error overlay for persistent failures (currently: repeated WiFi
// connect failure on a scheduled wake). Togglable via web UI and bot command.
void config_manager_set_error_overlay_enabled(bool enabled);
bool config_manager_get_error_overlay_enabled(void);

// Consecutive scheduled-wake WiFi connection failures (reset to 0 on any
// success). Persisted so it survives deep sleep between wakes.
void config_manager_set_wifi_fail_count(int count);
int config_manager_get_wifi_fail_count(void);

// ============================================================================
// WiFi
// ============================================================================

// When false, WiFi always stays in power-save mode regardless of the
// interactive/USB-triggered "full RX" policy in power_manager - lower draw,
// slower web UI. Defaults to true (existing tiered behavior unchanged).
void config_manager_set_wifi_performance_mode_enabled(bool enabled);
bool config_manager_get_wifi_performance_mode_enabled(void);

// Orientation pairing during normal (non-Telegram) auto-rotation - random
// mode only. Defaults to false. See NVS_ROTATION_PAIRING_ENABLED_KEY in
// config.h.
void config_manager_set_rotation_pairing_enabled(bool enabled);
bool config_manager_get_rotation_pairing_enabled(void);

// ============================================================================
// OTA
// ============================================================================

// Automatic OTA checks (periodic + cold-boot). A manual "check now" from the
// web UI is unaffected by this setting.
void config_manager_set_ota_check_enabled(bool enabled);
bool config_manager_get_ota_check_enabled(void);

// ============================================================================
// Power
// ============================================================================

void config_manager_set_deep_sleep_enabled(bool enabled);
bool config_manager_get_deep_sleep_enabled(void);

// ============================================================================
// Debugging
// ============================================================================

void config_manager_set_debug_log_enabled(bool enabled);
bool config_manager_get_debug_log_enabled(void);

// ============================================================================
// Config Sync
// ============================================================================

void config_manager_set_config_last_updated(int64_t timestamp);
int64_t config_manager_get_config_last_updated(void);
void config_manager_touch_config(void);

#endif
