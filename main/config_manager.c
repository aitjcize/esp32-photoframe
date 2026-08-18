#include "config_manager.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "board_hal.h"
#include "config.h"
#include "esp_log.h"
#include "nvs.h"
#include "storage.h"

static const char *TAG = "config_manager";

// General
static char device_name[DEVICE_NAME_MAX_LEN] = {0};
static char tz_string[TIMEZONE_MAX_LEN] = {0};
static display_orientation_t display_orientation = DISPLAY_ORIENTATION_LANDSCAPE;
static int display_rotation_deg = BOARD_HAL_DISPLAY_ROTATION_DEG;
static char wifi_ssid[WIFI_SSID_MAX_LEN] = {0};
static char wifi_password[WIFI_PASS_MAX_LEN] = {0};

// Advanced network settings (collapsed section in the UI): custom NTP server,
// static IP instead of DHCP, and DNS override
static char ntp_server[NTP_SERVER_MAX_LEN] = {0};
static ip_mode_t ip_mode = IP_MODE_DHCP;
static char static_ip[IP_ADDR_STR_MAX_LEN] = {0};
static char static_netmask[IP_ADDR_STR_MAX_LEN] = {0};
static char static_gateway[IP_ADDR_STR_MAX_LEN] = {0};
static char dns_server[IP_ADDR_STR_MAX_LEN] = {0};

// Auto Rotate
static bool auto_rotate_enabled = false;
static char cron_rules_store[MAX_CRON_RULES][CRON_RULE_MAX_LEN] = {{0}};
static int cron_rule_count = 0;

static rotation_mode_t rotation_mode =
    ROTATION_MODE_STORAGE;  // Default, will be validated during init

// Auto Rotate - SDCARD
static sd_rotation_mode_t sd_rotation_mode = SD_ROTATION_RANDOM;
static int32_t last_index = -1;

// Auto Rotate - URL
static char image_url[IMAGE_URL_MAX_LEN] = {0};
static uint8_t *ca_cert_der = NULL;  // Heap-allocated DER certificate
static size_t ca_cert_der_len = 0;
static char access_token[ACCESS_TOKEN_MAX_LEN] = {0};
static char http_header_key[HTTP_HEADER_KEY_MAX_LEN] = {0};
static char http_header_value[HTTP_HEADER_VALUE_MAX_LEN] = {0};
static bool save_downloaded_images = false;
static char image_etag[HTTP_ETAG_MAX_LEN] = {0};

// Home Assistant
static char ha_url[HA_URL_MAX_LEN] = {0};

// Telegram Bot
static char telegram_bot_token[TELEGRAM_BOT_TOKEN_MAX_LEN] = {0};
static char telegram_chat_id[TELEGRAM_CHAT_ID_MAX_LEN] = {0};
static int64_t telegram_last_update_id = 0;
static bool telegram_pairing_enabled = true;
static bool telegram_low_battery_warned = false;
static bool telegram_wake_notify_enabled = false;

typedef struct {
    char path[320];
    char caption[TELEGRAM_CAPTION_MAX_LEN];
} telegram_pending_image_t;
static telegram_pending_image_t telegram_pending_images[TELEGRAM_MAX_PENDING_IMAGES];
static int telegram_pending_image_count = 0;

// Home Assistant
static bool ha_enabled = false;

// Error overlay / WiFi failure tracking
static bool error_overlay_enabled = false;
static int wifi_fail_count = 0;

// WiFi
static bool wifi_performance_mode_enabled = true;
static bool rotation_pairing_enabled = false;
static bool telegram_rotation_notify_enabled = false;

// OTA
static bool ota_check_enabled = true;

// AI API Keys
static char openai_api_key[AI_API_KEY_MAX_LEN] = {0};
static char google_api_key[AI_API_KEY_MAX_LEN] = {0};

// Power
static bool deep_sleep_enabled = true;  // Enabled by default

// Debugging
static bool debug_log_enabled = false;

// Config sync
static int64_t config_last_updated = 0;

// ----------------------------------------------------------------------------
// Cron schedule helpers
// ----------------------------------------------------------------------------

// Fill cron_rules_store from a '\n'-separated joined string (skips empty and
// over-long entries, caps at MAX_CRON_RULES).
static void cron_load_from_joined(const char *joined)
{
    cron_rule_count = 0;
    if (!joined) {
        return;
    }
    const char *p = joined;
    while (*p && cron_rule_count < MAX_CRON_RULES) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t) (nl - p) : strlen(p);
        if (len > 0 && len < CRON_RULE_MAX_LEN) {
            memcpy(cron_rules_store[cron_rule_count], p, len);
            cron_rules_store[cron_rule_count][len] = '\0';
            cron_rule_count++;
        }
        if (!nl) {
            break;
        }
        p = nl + 1;
    }
}

// Persist the current cron_rules_store to NVS as a '\n'-joined string.
static void cron_persist(void)
{
    char joined[MAX_CRON_RULES * CRON_RULE_MAX_LEN];
    joined[0] = '\0';
    size_t off = 0;
    for (int i = 0; i < cron_rule_count; i++) {
        int n = snprintf(joined + off, sizeof(joined) - off, "%s%s", i ? "\n" : "",
                         cron_rules_store[i]);
        if (n < 0 || (size_t) n >= sizeof(joined) - off) {
            break;
        }
        off += n;
    }

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        if (cron_rule_count > 0) {
            nvs_set_str(nvs_handle, NVS_ROTATE_CRON_KEY, joined);
        } else {
            nvs_erase_key(nvs_handle, NVS_ROTATE_CRON_KEY);
        }
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }
}

// ----------------------------------------------------------------------------
// Telegram pending-image list helpers (queue of images waiting for an
// orientation-pairing partner). Joined-string NVS encoding mirrors the cron
// helpers above: 0x1F separates path/caption within an entry, 0x1E separates
// entries. Those control bytes can't appear in normal text, so no escaping is
// needed even though captions may contain arbitrary UTF-8/newlines.
// ----------------------------------------------------------------------------

#define TELEGRAM_PENDING_JOINED_MAX \
    (TELEGRAM_MAX_PENDING_IMAGES * (sizeof(((telegram_pending_image_t *) 0)->path) + \
                                    TELEGRAM_CAPTION_MAX_LEN + 2))

static void telegram_pending_persist(void)
{
    // static, not a stack local: this project's main task stack is only
    // 6144 bytes and this buffer (TELEGRAM_MAX_PENDING_IMAGES * ~450 bytes)
    // is called from deep within the Telegram poll's call chain, where
    // stack headroom is already tight. config_manager runs single-threaded
    // (main task only), so a static buffer here is safe.
    static char joined[TELEGRAM_PENDING_JOINED_MAX];
    size_t off = 0;
    joined[0] = '\0';
    for (int i = 0; i < telegram_pending_image_count; i++) {
        int n = snprintf(joined + off, sizeof(joined) - off, "%s\x1F%s\x1E",
                         telegram_pending_images[i].path, telegram_pending_images[i].caption);
        if (n < 0 || (size_t) n >= sizeof(joined) - off) {
            break;
        }
        off += (size_t) n;
    }

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        if (telegram_pending_image_count > 0) {
            nvs_set_str(nvs_handle, NVS_TELEGRAM_PENDING_LIST_KEY, joined);
        } else {
            nvs_erase_key(nvs_handle, NVS_TELEGRAM_PENDING_LIST_KEY);
        }
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }
}

static void telegram_pending_load_from_joined(const char *joined)
{
    telegram_pending_image_count = 0;
    if (!joined) {
        return;
    }
    const char *p = joined;
    while (*p != '\0' && telegram_pending_image_count < TELEGRAM_MAX_PENDING_IMAGES) {
        const char *rec_end = strchr(p, '\x1E');
        size_t rec_len = rec_end ? (size_t) (rec_end - p) : strlen(p);
        const char *sep = memchr(p, '\x1F', rec_len);
        if (sep) {
            telegram_pending_image_t *e = &telegram_pending_images[telegram_pending_image_count];
            size_t path_len = (size_t) (sep - p);
            if (path_len >= sizeof(e->path)) {
                path_len = sizeof(e->path) - 1;
            }
            memcpy(e->path, p, path_len);
            e->path[path_len] = '\0';

            size_t cap_len = rec_len - (size_t) (sep - p) - 1;
            if (cap_len >= sizeof(e->caption)) {
                cap_len = sizeof(e->caption) - 1;
            }
            memcpy(e->caption, sep + 1, cap_len);
            e->caption[cap_len] = '\0';

            telegram_pending_image_count++;
        }
        if (!rec_end) {
            break;
        }
        p = rec_end + 1;
    }
}

// Convert a legacy rotation interval (seconds) into a single cron expression.
// Mirrors the documented best-effort mapping; falls back to hourly.
static void cron_from_legacy_interval(int seconds, char *out, size_t out_len)
{
    if (seconds >= 3600 && seconds % 3600 == 0) {
        int hours = seconds / 3600;
        if (hours <= 1) {
            snprintf(out, out_len, "0 * *");
        } else if (hours >= 24) {
            snprintf(out, out_len, "0 0 *");
        } else {
            snprintf(out, out_len, "0 */%d *", hours);
        }
    } else if (seconds >= 60 && 3600 % seconds == 0) {
        snprintf(out, out_len, "*/%d * *", seconds / 60);
    } else {
        // Not cleanly expressible as cron (e.g. 90 min) — approximate to hourly.
        snprintf(out, out_len, "0 * *");
    }
}

esp_err_t config_manager_init(void)
{
    ESP_LOGI(TAG, "Initializing config manager");

    // Rotation-schedule load is resolved after the read-only NVS handle closes
    // (migration / default seeding may need a read-write handle).
    char cron_buf[MAX_CRON_RULES * CRON_RULE_MAX_LEN] = {0};
    int32_t legacy_interval = 0;
    bool migrate_legacy_interval = false;
    bool seed_default_cron = true;

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle) == ESP_OK) {
        // General
        size_t device_name_len = DEVICE_NAME_MAX_LEN;
        if (nvs_get_str(nvs_handle, NVS_DEVICE_NAME_KEY, device_name, &device_name_len) == ESP_OK) {
            ESP_LOGI(TAG, "Loaded device name from NVS: %s", device_name);
        } else {
            strncpy(device_name, DEFAULT_DEVICE_NAME, DEVICE_NAME_MAX_LEN - 1);
            device_name[DEVICE_NAME_MAX_LEN - 1] = '\0';
            ESP_LOGI(TAG, "No device name in NVS, using default: %s", device_name);
        }

        size_t tz_len = TIMEZONE_MAX_LEN;
        if (nvs_get_str(nvs_handle, NVS_TIMEZONE_KEY, tz_string, &tz_len) == ESP_OK) {
            ESP_LOGI(TAG, "Loaded timezone from NVS: %s", tz_string);
        } else {
            strncpy(tz_string, DEFAULT_TIMEZONE, TIMEZONE_MAX_LEN - 1);
            tz_string[TIMEZONE_MAX_LEN - 1] = '\0';
            ESP_LOGI(TAG, "No timezone in NVS, using default: %s", tz_string);
        }

        size_t ntp_server_len = NTP_SERVER_MAX_LEN;
        if (nvs_get_str(nvs_handle, NVS_NTP_SERVER_KEY, ntp_server, &ntp_server_len) == ESP_OK) {
            ESP_LOGI(TAG, "Loaded NTP server from NVS: %s", ntp_server);
        } else {
            strncpy(ntp_server, DEFAULT_NTP_SERVER, NTP_SERVER_MAX_LEN - 1);
            ntp_server[NTP_SERVER_MAX_LEN - 1] = '\0';
            ESP_LOGI(TAG, "No NTP server in NVS, using default: %s", ntp_server);
        }

        // Advanced network settings
        uint8_t stored_ip_mode = IP_MODE_DHCP;
        if (nvs_get_u8(nvs_handle, NVS_IP_MODE_KEY, &stored_ip_mode) == ESP_OK) {
            ip_mode = (ip_mode_t) stored_ip_mode;
        }
        size_t addr_len = sizeof(static_ip);
        nvs_get_str(nvs_handle, NVS_STATIC_IP_KEY, static_ip, &addr_len);
        addr_len = sizeof(static_netmask);
        nvs_get_str(nvs_handle, NVS_STATIC_NETMASK_KEY, static_netmask, &addr_len);
        addr_len = sizeof(static_gateway);
        nvs_get_str(nvs_handle, NVS_STATIC_GATEWAY_KEY, static_gateway, &addr_len);
        addr_len = sizeof(dns_server);
        nvs_get_str(nvs_handle, NVS_DNS_SERVER_KEY, dns_server, &addr_len);
        if (ip_mode == IP_MODE_STATIC) {
            ESP_LOGI(TAG, "Static IP configured: %s/%s gw %s dns %s", static_ip, static_netmask,
                     static_gateway, dns_server[0] ? dns_server : "(auto)");
        } else if (dns_server[0]) {
            ESP_LOGI(TAG, "DNS override configured: %s", dns_server);
        }

        uint8_t stored_orientation = DISPLAY_ORIENTATION_LANDSCAPE;
        if (nvs_get_u8(nvs_handle, NVS_DISPLAY_ORIENTATION_KEY, &stored_orientation) == ESP_OK) {
            display_orientation = (display_orientation_t) stored_orientation;
            ESP_LOGI(
                TAG, "Loaded display orientation from NVS: %s",
                display_orientation == DISPLAY_ORIENTATION_LANDSCAPE ? "landscape" : "portrait");
        }

        int32_t stored_display_rotation_deg = 0;
        if (nvs_get_i32(nvs_handle, NVS_DISPLAY_ROTATION_DEG_KEY, &stored_display_rotation_deg) ==
            ESP_OK) {
            // Only 0/180 are supported; a legacy 90/270 value would swap the
            // paint geometry away from the native layout the streaming
            // pipeline assumes (see apply_config_from_json). Keep the board
            // default instead.
            if (stored_display_rotation_deg == 0 || stored_display_rotation_deg == 180) {
                display_rotation_deg = stored_display_rotation_deg;
                ESP_LOGI(TAG, "Loaded display rotation from NVS: %d degrees", display_rotation_deg);
            } else {
                ESP_LOGW(TAG, "Ignoring unsupported stored display rotation %ld degrees",
                         (long) stored_display_rotation_deg);
            }
        }

        size_t wifi_ssid_len = WIFI_SSID_MAX_LEN;
        if (nvs_get_str(nvs_handle, NVS_WIFI_SSID_KEY, wifi_ssid, &wifi_ssid_len) == ESP_OK) {
            ESP_LOGI(TAG, "Loaded WiFi SSID from NVS: %s", wifi_ssid);
        } else {
            strncpy(wifi_ssid, DEFAULT_WIFI_SSID, WIFI_SSID_MAX_LEN - 1);
            wifi_ssid[WIFI_SSID_MAX_LEN - 1] = '\0';
            ESP_LOGI(TAG, "No WiFi SSID in NVS, using default: %s", wifi_ssid);
        }

        size_t wifi_pass_len = WIFI_PASS_MAX_LEN;
        if (nvs_get_str(nvs_handle, NVS_WIFI_PASS_KEY, wifi_password, &wifi_pass_len) == ESP_OK) {
            ESP_LOGI(TAG, "Loaded WiFi password from NVS (length: %zu)", wifi_pass_len);
        } else {
            strncpy(wifi_password, DEFAULT_WIFI_PASSWORD, WIFI_PASS_MAX_LEN - 1);
            wifi_password[WIFI_PASS_MAX_LEN - 1] = '\0';
            ESP_LOGI(TAG, "No WiFi password in NVS, using default");
        }

        // Auto Rotate
        uint8_t stored_enabled = 0;
        if (nvs_get_u8(nvs_handle, NVS_AUTO_ROTATE_KEY, &stored_enabled) == ESP_OK) {
            auto_rotate_enabled = (stored_enabled != 0);
            ESP_LOGI(TAG, "Loaded auto-rotate enabled from NVS: %s",
                     auto_rotate_enabled ? "yes" : "no");
        }

        // Rotation schedule (cron). If absent, fall back to migrating a legacy
        // interval, else seed the default — both handled after this handle closes.
        size_t cron_len = sizeof(cron_buf);
        if (nvs_get_str(nvs_handle, NVS_ROTATE_CRON_KEY, cron_buf, &cron_len) == ESP_OK) {
            cron_load_from_joined(cron_buf);
            seed_default_cron = false;
            ESP_LOGI(TAG, "Loaded %d cron rule(s) from NVS", cron_rule_count);
        } else if (nvs_get_i32(nvs_handle, NVS_ROTATE_INTERVAL_KEY, &legacy_interval) == ESP_OK) {
            migrate_legacy_interval = true;
            seed_default_cron = false;
        }

        uint8_t stored_mode = ROTATION_MODE_URL;  // Default fallback
        if (nvs_get_u8(nvs_handle, NVS_ROTATION_MODE_KEY, &stored_mode) == ESP_OK) {
            rotation_mode = (rotation_mode_t) stored_mode;
            ESP_LOGI(TAG, "Loaded rotation mode from NVS: %s",
                     rotation_mode == ROTATION_MODE_URL ? "url" : "storage");
        } else if (storage_has_persistent_storage()) {
            rotation_mode = ROTATION_MODE_STORAGE;
            ESP_LOGI(TAG, "No rotation mode in NVS, using default for persistent storage: storage");
        } else {
            ESP_LOGI(TAG, "No rotation mode in NVS, using default for no-storage: url");
        }

        // Auto Rotate - SDCARD
        uint8_t stored_sd_mode = SD_ROTATION_RANDOM;
        if (nvs_get_u8(nvs_handle, NVS_SD_ROTATION_MODE_KEY, &stored_sd_mode) == ESP_OK) {
            sd_rotation_mode = (sd_rotation_mode_t) stored_sd_mode;
            ESP_LOGI(TAG, "Loaded SD rotation mode from NVS: %s",
                     sd_rotation_mode == SD_ROTATION_SEQUENTIAL ? "sequential" : "random");
        }

        int32_t stored_last_index = -1;
        if (nvs_get_i32(nvs_handle, NVS_LAST_INDEX_KEY, &stored_last_index) == ESP_OK) {
            last_index = stored_last_index;
            ESP_LOGI(TAG, "Loaded last index from NVS: %ld", (long) last_index);
        }

        // Auto Rotate - URL
        size_t url_len = IMAGE_URL_MAX_LEN;
        if (nvs_get_str(nvs_handle, NVS_IMAGE_URL_KEY, image_url, &url_len) == ESP_OK) {
            ESP_LOGI(TAG, "Loaded image URL from NVS: %s", image_url);
        } else {
            strncpy(image_url, DEFAULT_IMAGE_URL, IMAGE_URL_MAX_LEN - 1);
            image_url[IMAGE_URL_MAX_LEN - 1] = '\0';
            ESP_LOGI(TAG, "No image URL in NVS, using default: %s", image_url);
        }

        // CA Certificate DER blob (heap-allocated)
        size_t blob_len = 0;
        if (nvs_get_blob(nvs_handle, NVS_CA_CERT_KEY, NULL, &blob_len) == ESP_OK && blob_len > 0) {
            ca_cert_der = malloc(blob_len);
            if (ca_cert_der &&
                nvs_get_blob(nvs_handle, NVS_CA_CERT_KEY, ca_cert_der, &blob_len) == ESP_OK) {
                ca_cert_der_len = blob_len;
                ESP_LOGI(TAG, "Loaded CA certificate from NVS (%zu bytes)", ca_cert_der_len);
            } else {
                free(ca_cert_der);
                ca_cert_der = NULL;
                ca_cert_der_len = 0;
            }
        }

        size_t access_token_len = ACCESS_TOKEN_MAX_LEN;
        if (nvs_get_str(nvs_handle, NVS_ACCESS_TOKEN_KEY, access_token, &access_token_len) ==
            ESP_OK) {
            ESP_LOGI(TAG, "Loaded access token from NVS (length: %zu)", access_token_len);
        }

        size_t http_header_key_len = HTTP_HEADER_KEY_MAX_LEN;
        if (nvs_get_str(nvs_handle, NVS_HTTP_HEADER_KEY_KEY, http_header_key,
                        &http_header_key_len) == ESP_OK) {
            ESP_LOGI(TAG, "Loaded HTTP header key from NVS: %s", http_header_key);
        }

        size_t http_header_value_len = HTTP_HEADER_VALUE_MAX_LEN;
        if (nvs_get_str(nvs_handle, NVS_HTTP_HEADER_VALUE_KEY, http_header_value,
                        &http_header_value_len) == ESP_OK) {
            ESP_LOGI(TAG, "Loaded HTTP header value from NVS (length: %zu)", http_header_value_len);
        }

        uint8_t stored_save_dl = 0;
        if (nvs_get_u8(nvs_handle, NVS_SAVE_DOWNLOADED_KEY, &stored_save_dl) == ESP_OK) {
            save_downloaded_images = (stored_save_dl != 0);
            ESP_LOGI(TAG, "Loaded save_downloaded_images from NVS: %s",
                     save_downloaded_images ? "yes" : "no");
        }

        size_t etag_len = HTTP_ETAG_MAX_LEN;
        if (nvs_get_str(nvs_handle, NVS_IMAGE_ETAG_KEY, image_etag, &etag_len) == ESP_OK) {
            ESP_LOGI(TAG, "Loaded image ETag from NVS (length: %zu)", etag_len);
        }

        // Home Assistant
        size_t ha_url_len = HA_URL_MAX_LEN;
        if (nvs_get_str(nvs_handle, NVS_HA_URL_KEY, ha_url, &ha_url_len) == ESP_OK) {
            ESP_LOGI(TAG, "Loaded HA URL from NVS: %s", ha_url);
        } else {
            strncpy(ha_url, DEFAULT_HA_URL, HA_URL_MAX_LEN - 1);
            ha_url[HA_URL_MAX_LEN - 1] = '\0';
            ESP_LOGI(TAG, "No HA URL in NVS, using default (empty)");
        }

        uint8_t stored_ha_enabled;
        if (nvs_get_u8(nvs_handle, NVS_HA_ENABLED_KEY, &stored_ha_enabled) == ESP_OK) {
            ha_enabled = (stored_ha_enabled != 0);
        } else {
            // No explicit setting yet: preserve pre-existing behavior for a
            // device that already had an HA URL configured before this
            // switch existed; fresh/factory-reset devices default to off.
            ha_enabled = (ha_url[0] != '\0');
        }
        ESP_LOGI(TAG, "Home Assistant integration: %s", ha_enabled ? "enabled" : "disabled");

        // Telegram Bot
        size_t tg_token_len = TELEGRAM_BOT_TOKEN_MAX_LEN;
        if (nvs_get_str(nvs_handle, NVS_TELEGRAM_BOT_TOKEN_KEY, telegram_bot_token,
                        &tg_token_len) == ESP_OK) {
            ESP_LOGI(TAG, "Loaded Telegram bot token from NVS (length: %zu)", tg_token_len);
        }

        size_t tg_chat_id_len = TELEGRAM_CHAT_ID_MAX_LEN;
        if (nvs_get_str(nvs_handle, NVS_TELEGRAM_CHAT_ID_KEY, telegram_chat_id, &tg_chat_id_len) ==
            ESP_OK) {
            ESP_LOGI(TAG, "Loaded Telegram chat ID from NVS (length: %zu)", tg_chat_id_len);
        }

        if (nvs_get_i64(nvs_handle, NVS_TELEGRAM_LAST_UPDATE_ID_KEY, &telegram_last_update_id) ==
            ESP_OK) {
            ESP_LOGI(TAG, "Loaded Telegram last update_id from NVS: %lld",
                     (long long) telegram_last_update_id);
        }

        uint8_t stored_pairing = 1;  // Default to enabled
        if (nvs_get_u8(nvs_handle, NVS_TELEGRAM_PAIRING_KEY, &stored_pairing) == ESP_OK) {
            telegram_pairing_enabled = (stored_pairing != 0);
        }
        ESP_LOGI(TAG, "Telegram orientation pairing: %s",
                 telegram_pairing_enabled ? "enabled" : "disabled");

        uint8_t stored_low_batt_warned = 0;
        if (nvs_get_u8(nvs_handle, NVS_TELEGRAM_LOW_BATT_WARNED_KEY, &stored_low_batt_warned) ==
            ESP_OK) {
            telegram_low_battery_warned = (stored_low_batt_warned != 0);
        }

        uint8_t stored_wake_notify = 0;
        if (nvs_get_u8(nvs_handle, NVS_TELEGRAM_WAKE_NOTIFY_KEY, &stored_wake_notify) == ESP_OK) {
            telegram_wake_notify_enabled = (stored_wake_notify != 0);
        }

        uint8_t stored_error_overlay = 0;
        if (nvs_get_u8(nvs_handle, NVS_ERROR_OVERLAY_ENABLED_KEY, &stored_error_overlay) ==
            ESP_OK) {
            error_overlay_enabled = (stored_error_overlay != 0);
        }

        int32_t stored_wifi_fail_count = 0;
        if (nvs_get_i32(nvs_handle, NVS_WIFI_FAIL_COUNT_KEY, &stored_wifi_fail_count) == ESP_OK) {
            wifi_fail_count = (int) stored_wifi_fail_count;
        }

        uint8_t stored_wifi_perf = 1;  // Default to enabled (existing tiered behavior)
        if (nvs_get_u8(nvs_handle, NVS_WIFI_PERF_MODE_ENABLED_KEY, &stored_wifi_perf) == ESP_OK) {
            wifi_performance_mode_enabled = (stored_wifi_perf != 0);
        }

        uint8_t stored_rotation_pairing = 0;
        if (nvs_get_u8(nvs_handle, NVS_ROTATION_PAIRING_ENABLED_KEY, &stored_rotation_pairing) ==
            ESP_OK) {
            rotation_pairing_enabled = (stored_rotation_pairing != 0);
        }

        uint8_t stored_rotation_notify = 0;
        if (nvs_get_u8(nvs_handle, NVS_TELEGRAM_ROTATION_NOTIFY_KEY, &stored_rotation_notify) ==
            ESP_OK) {
            telegram_rotation_notify_enabled = (stored_rotation_notify != 0);
        }

        {
            static char pending_buf[TELEGRAM_PENDING_JOINED_MAX];
            pending_buf[0] = '\0';
            size_t pending_len = sizeof(pending_buf);
            if (nvs_get_str(nvs_handle, NVS_TELEGRAM_PENDING_LIST_KEY, pending_buf, &pending_len) ==
                ESP_OK) {
                telegram_pending_load_from_joined(pending_buf);
                ESP_LOGI(TAG, "Loaded %d pending Telegram pair image(s) from NVS",
                         telegram_pending_image_count);
            }
        }

        uint8_t stored_ota_check = 1;  // Default to enabled (unchanged prior behavior)
        if (nvs_get_u8(nvs_handle, NVS_OTA_CHECK_ENABLED_KEY, &stored_ota_check) == ESP_OK) {
            ota_check_enabled = (stored_ota_check != 0);
        }
        ESP_LOGI(TAG, "Automatic OTA check: %s", ota_check_enabled ? "enabled" : "disabled");

        // AI API Keys
        size_t openai_key_len = AI_API_KEY_MAX_LEN;
        if (nvs_get_str(nvs_handle, NVS_OPENAI_API_KEY_KEY, openai_api_key, &openai_key_len) ==
            ESP_OK) {
            ESP_LOGI(TAG, "Loaded OpenAI API Key from NVS");
        }

        size_t google_key_len = AI_API_KEY_MAX_LEN;
        if (nvs_get_str(nvs_handle, NVS_GOOGLE_API_KEY_KEY, google_api_key, &google_key_len) ==
            ESP_OK) {
            ESP_LOGI(TAG, "Loaded Google API Key from NVS");
        }

        // Power
        uint8_t deep_sleep_val = 1;  // Default to enabled
        if (nvs_get_u8(nvs_handle, NVS_DEEP_SLEEP_KEY, &deep_sleep_val) == ESP_OK) {
            deep_sleep_enabled = (deep_sleep_val != 0);
            ESP_LOGI(TAG, "Loaded deep sleep setting from NVS: %s",
                     deep_sleep_enabled ? "enabled" : "disabled");
        }

        // Debugging
        uint8_t debug_log_val = 0;
        if (nvs_get_u8(nvs_handle, NVS_DEBUG_LOG_KEY, &debug_log_val) == ESP_OK) {
            debug_log_enabled = (debug_log_val != 0);
            ESP_LOGI(TAG, "Loaded debug log setting from NVS: %s",
                     debug_log_enabled ? "enabled" : "disabled");
        }

        // Config sync timestamp
        if (nvs_get_i64(nvs_handle, "cfg_updated", &config_last_updated) == ESP_OK) {
            ESP_LOGI(TAG, "Loaded config_last_updated: %lld", (long long) config_last_updated);
        }

        nvs_close(nvs_handle);
    }

    // Resolve the rotation schedule now that the read-only handle is closed.
    if (migrate_legacy_interval) {
        char rule[CRON_RULE_MAX_LEN];
        cron_from_legacy_interval((int) legacy_interval, rule, sizeof(rule));
        const char *one[1] = {rule};
        config_manager_set_cron_rules(one, 1);  // persists to NVS
        ESP_LOGI(TAG, "Migrated legacy interval %d s -> cron \"%s\"", (int) legacy_interval, rule);
    } else if (seed_default_cron) {
        // Fresh device: seed default in memory; persists on the first user save.
        cron_load_from_joined(DEFAULT_ROTATE_CRON);
        ESP_LOGI(TAG, "No rotation schedule in NVS, using default: %s", DEFAULT_ROTATE_CRON);
    }

    // Erase the legacy quiet-hours (sleep schedule) keys. That feature was
    // replaced by cron rules that carry their own active-hours window; the
    // firmware no longer reads these keys, so drop them from NVS.
    {
        nvs_handle_t erase_handle;
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &erase_handle) == ESP_OK) {
            bool erased = false;
            const char *legacy_keys[] = {
                NVS_SLEEP_SCHEDULE_ENABLED_KEY,
                NVS_SLEEP_SCHEDULE_START_KEY,
                NVS_SLEEP_SCHEDULE_END_KEY,
            };
            for (size_t i = 0; i < sizeof(legacy_keys) / sizeof(legacy_keys[0]); i++) {
                if (nvs_erase_key(erase_handle, legacy_keys[i]) == ESP_OK) {
                    erased = true;
                }
            }
            if (erased) {
                nvs_commit(erase_handle);
                ESP_LOGI(TAG, "Erased legacy sleep-schedule keys from NVS");
            }
            nvs_close(erase_handle);
        }
    }

    // Apply timezone setting
    setenv("TZ", tz_string, 1);
    tzset();
    ESP_LOGI(TAG, "Timezone set to: %s", tz_string);

    // Log current system time in local timezone
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    char strftime_buf[64];
    strftime(strftime_buf, sizeof(strftime_buf), "%Y-%m-%d %H:%M:%S", &timeinfo);

    // Calculate UTC offset for display
    struct tm utc_timeinfo;
    gmtime_r(&now, &utc_timeinfo);
    int offset_hours = timeinfo.tm_hour - utc_timeinfo.tm_hour;

    // Handle day boundary crossing
    if (offset_hours > 12)
        offset_hours -= 24;
    if (offset_hours < -12)
        offset_hours += 24;

    ESP_LOGI(TAG, "Config manager initialized");
    return ESP_OK;
}
// ============================================================================
// General
// ============================================================================

void config_manager_set_device_name(const char *name)
{
    if (name == NULL) {
        return;
    }

    strncpy(device_name, name, DEVICE_NAME_MAX_LEN - 1);
    device_name[DEVICE_NAME_MAX_LEN - 1] = '\0';

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_str(nvs_handle, NVS_DEVICE_NAME_KEY, device_name);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Device name set to: %s", device_name);
}
const char *config_manager_get_device_name(void)
{
    return device_name;
}

void config_manager_set_timezone(const char *tz)
{
    if (tz == NULL) {
        return;
    }

    strncpy(tz_string, tz, TIMEZONE_MAX_LEN - 1);
    tz_string[TIMEZONE_MAX_LEN - 1] = '\0';

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_str(nvs_handle, NVS_TIMEZONE_KEY, tz_string);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Timezone set to: %s", tz_string);
}
const char *config_manager_get_timezone(void)
{
    if (tz_string[0] == '\0') {
        return "UTC0";
    }
    return tz_string;
}

void config_manager_set_ntp_server(const char *server)
{
    if (server == NULL) {
        return;
    }

    strncpy(ntp_server, server, NTP_SERVER_MAX_LEN - 1);
    ntp_server[NTP_SERVER_MAX_LEN - 1] = '\0';

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_str(nvs_handle, NVS_NTP_SERVER_KEY, ntp_server);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "NTP server set to: %s", ntp_server);
}

const char *config_manager_get_ntp_server(void)
{
    if (ntp_server[0] == '\0') {
        return DEFAULT_NTP_SERVER;
    }
    return ntp_server;
}

// ----------------------------------------------------------------------------
// Advanced network settings
// ----------------------------------------------------------------------------

// Persist one dotted-IPv4 string setting (helper for the network settings).
static void set_ip_str(const char *nvs_key, char *cache, const char *value)
{
    if (value == NULL) {
        value = "";
    }
    strncpy(cache, value, IP_ADDR_STR_MAX_LEN - 1);
    cache[IP_ADDR_STR_MAX_LEN - 1] = '\0';

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_str(nvs_handle, nvs_key, cache);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }
}

void config_manager_set_ip_mode(ip_mode_t mode)
{
    ip_mode = mode;

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_u8(nvs_handle, NVS_IP_MODE_KEY, (uint8_t) mode);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }
    ESP_LOGI(TAG, "IP mode set to: %s", mode == IP_MODE_STATIC ? "static" : "dhcp");
}

ip_mode_t config_manager_get_ip_mode(void)
{
    return ip_mode;
}

void config_manager_set_static_ip(const char *ip)
{
    set_ip_str(NVS_STATIC_IP_KEY, static_ip, ip);
}

const char *config_manager_get_static_ip(void)
{
    return static_ip;
}

void config_manager_set_static_netmask(const char *mask)
{
    set_ip_str(NVS_STATIC_NETMASK_KEY, static_netmask, mask);
}

const char *config_manager_get_static_netmask(void)
{
    return static_netmask;
}

void config_manager_set_static_gateway(const char *gw)
{
    set_ip_str(NVS_STATIC_GATEWAY_KEY, static_gateway, gw);
}

const char *config_manager_get_static_gateway(void)
{
    return static_gateway;
}

void config_manager_set_dns_server(const char *dns)
{
    set_ip_str(NVS_DNS_SERVER_KEY, dns_server, dns);
}

const char *config_manager_get_dns_server(void)
{
    return dns_server;
}

void config_manager_set_display_orientation(display_orientation_t orientation)
{
    display_orientation = orientation;

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_u8(nvs_handle, NVS_DISPLAY_ORIENTATION_KEY, (uint8_t) orientation);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Display orientation set to: %s",
             orientation == DISPLAY_ORIENTATION_LANDSCAPE ? "landscape" : "portrait");
}

display_orientation_t config_manager_get_display_orientation(void)
{
    return display_orientation;
}

void config_manager_set_display_rotation_deg(int rotation_deg)
{
    display_rotation_deg = rotation_deg;

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_i32(nvs_handle, NVS_DISPLAY_ROTATION_DEG_KEY, rotation_deg);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Display rotation set to %d degrees", rotation_deg);
}

int config_manager_get_display_rotation_deg(void)
{
    return display_rotation_deg;
}

void config_manager_set_wifi_ssid(const char *ssid)
{
    if (ssid == NULL) {
        return;
    }

    strncpy(wifi_ssid, ssid, WIFI_SSID_MAX_LEN - 1);
    wifi_ssid[WIFI_SSID_MAX_LEN - 1] = '\0';

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_str(nvs_handle, NVS_WIFI_SSID_KEY, wifi_ssid);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "WiFi SSID set to: %s", wifi_ssid);
}

const char *config_manager_get_wifi_ssid(void)
{
    return wifi_ssid;
}

void config_manager_set_wifi_password(const char *password)
{
    if (password == NULL) {
        return;
    }

    strncpy(wifi_password, password, WIFI_PASS_MAX_LEN - 1);
    wifi_password[WIFI_PASS_MAX_LEN - 1] = '\0';

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_str(nvs_handle, NVS_WIFI_PASS_KEY, wifi_password);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "WiFi password set (length: %zu)", strlen(wifi_password));
}

const char *config_manager_get_wifi_password(void)
{
    return wifi_password;
}
// ============================================================================
// Auto Rotate
// ============================================================================

void config_manager_set_auto_rotate(bool enabled)
{
    auto_rotate_enabled = enabled;

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_u8(nvs_handle, NVS_AUTO_ROTATE_KEY, enabled ? 1 : 0);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Auto-rotate %s", enabled ? "enabled" : "disabled");
}

bool config_manager_get_auto_rotate(void)
{
    return auto_rotate_enabled;
}

int config_manager_get_cron_rule_count(void)
{
    return cron_rule_count;
}

const char *config_manager_get_cron_rule(int index)
{
    if (index < 0 || index >= cron_rule_count) {
        return NULL;
    }
    return cron_rules_store[index];
}

void config_manager_set_cron_rules(const char *const *rules, int count)
{
    if (count < 0) {
        count = 0;
    }
    cron_rule_count = 0;
    for (int i = 0; i < count && cron_rule_count < MAX_CRON_RULES; i++) {
        if (!rules[i] || rules[i][0] == '\0' || strlen(rules[i]) >= CRON_RULE_MAX_LEN) {
            continue;
        }
        strncpy(cron_rules_store[cron_rule_count], rules[i], CRON_RULE_MAX_LEN - 1);
        cron_rules_store[cron_rule_count][CRON_RULE_MAX_LEN - 1] = '\0';
        cron_rule_count++;
    }

    cron_persist();
    ESP_LOGI(TAG, "Rotation schedule set to %d cron rule(s)", cron_rule_count);
}

void config_manager_set_cron_rules_from_interval(int seconds)
{
    char rule[CRON_RULE_MAX_LEN];
    cron_from_legacy_interval(seconds, rule, sizeof(rule));
    const char *one[1] = {rule};
    config_manager_set_cron_rules(one, 1);
}

int config_manager_get_compiled_cron_rules(cron_rule_t *out, int max)
{
    int n = 0;
    for (int i = 0; i < cron_rule_count && n < max; i++) {
        if (cron_parse(cron_rules_store[i], &out[n])) {
            n++;
        }
    }
    return n;
}

void config_manager_set_rotation_mode(rotation_mode_t mode)
{
    if (!storage_has_persistent_storage() && mode == ROTATION_MODE_STORAGE) {
        ESP_LOGE(TAG, "Cannot set rotation mode to STORAGE: Local storage not supported");
        return;
    }

    rotation_mode = mode;

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_u8(nvs_handle, NVS_ROTATION_MODE_KEY, (uint8_t) mode);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Rotation mode set to: %s", mode == ROTATION_MODE_URL ? "url" : "sdcard");
}

rotation_mode_t config_manager_get_rotation_mode(void)
{
    return rotation_mode;
}
// ============================================================================
// Auto Rotate - SDCARD
// ============================================================================

void config_manager_set_sd_rotation_mode(sd_rotation_mode_t mode)
{
    sd_rotation_mode = mode;

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_u8(nvs_handle, NVS_SD_ROTATION_MODE_KEY, (uint8_t) mode);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "SD rotation mode set to: %s",
             mode == SD_ROTATION_SEQUENTIAL ? "sequential" : "random");
}

sd_rotation_mode_t config_manager_get_sd_rotation_mode(void)
{
    return sd_rotation_mode;
}

void config_manager_set_last_index(int32_t index)
{
    last_index = index;

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_i32(nvs_handle, NVS_LAST_INDEX_KEY, index);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }
}

int32_t config_manager_get_last_index(void)
{
    return last_index;
}

// ============================================================================
// Auto Rotate - URL
// ============================================================================

void config_manager_set_image_url(const char *url)
{
    const char *new_url = url ? url : "";
    bool url_changed = strcmp(image_url, new_url) != 0;

    strncpy(image_url, new_url, IMAGE_URL_MAX_LEN - 1);
    image_url[IMAGE_URL_MAX_LEN - 1] = '\0';

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        if (image_url[0] != '\0') {
            nvs_set_str(nvs_handle, NVS_IMAGE_URL_KEY, image_url);
        } else {
            nvs_erase_key(nvs_handle, NVS_IMAGE_URL_KEY);
        }
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    if (url_changed) {
        config_manager_set_image_etag("");
    }

    ESP_LOGI(TAG, "Image URL set to: %s", image_url[0] ? image_url : "(empty)");
}
const char *config_manager_get_image_url(void)
{
    return image_url;
}

void config_manager_set_ca_cert_der(const uint8_t *der, size_t len)
{
    free(ca_cert_der);
    ca_cert_der = NULL;
    ca_cert_der_len = 0;

    if (der && len > 0) {
        ca_cert_der = malloc(len);
        if (ca_cert_der) {
            memcpy(ca_cert_der, der, len);
            ca_cert_der_len = len;
        }
    }

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        if (ca_cert_der) {
            nvs_set_blob(nvs_handle, NVS_CA_CERT_KEY, ca_cert_der, ca_cert_der_len);
        } else {
            nvs_erase_key(nvs_handle, NVS_CA_CERT_KEY);
        }
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "CA certificate %s (%zu bytes)", ca_cert_der ? "set" : "cleared",
             ca_cert_der_len);
}

const uint8_t *config_manager_get_ca_cert_der(size_t *out_len)
{
    if (out_len) {
        *out_len = ca_cert_der_len;
    }
    return ca_cert_der;
}

void config_manager_set_access_token(const char *token)
{
    if (token == NULL) {
        return;
    }

    strncpy(access_token, token, ACCESS_TOKEN_MAX_LEN - 1);
    access_token[ACCESS_TOKEN_MAX_LEN - 1] = '\0';

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_str(nvs_handle, NVS_ACCESS_TOKEN_KEY, access_token);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Access token set (length: %zu)", strlen(access_token));
}
const char *config_manager_get_access_token(void)
{
    return access_token;
}

void config_manager_set_http_header_key(const char *key)
{
    if (key == NULL) {
        return;
    }

    strncpy(http_header_key, key, HTTP_HEADER_KEY_MAX_LEN - 1);
    http_header_key[HTTP_HEADER_KEY_MAX_LEN - 1] = '\0';

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_str(nvs_handle, NVS_HTTP_HEADER_KEY_KEY, http_header_key);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "HTTP header key set to: %s", http_header_key);
}
const char *config_manager_get_http_header_key(void)
{
    return http_header_key;
}

void config_manager_set_http_header_value(const char *value)
{
    if (value == NULL) {
        return;
    }

    strncpy(http_header_value, value, HTTP_HEADER_VALUE_MAX_LEN - 1);
    http_header_value[HTTP_HEADER_VALUE_MAX_LEN - 1] = '\0';

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_str(nvs_handle, NVS_HTTP_HEADER_VALUE_KEY, http_header_value);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "HTTP header value set (length: %zu)", strlen(http_header_value));
}
const char *config_manager_get_http_header_value(void)
{
    return http_header_value;
}

void config_manager_set_save_downloaded_images(bool enabled)
{
    save_downloaded_images = enabled;

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_u8(nvs_handle, NVS_SAVE_DOWNLOADED_KEY, enabled ? 1 : 0);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Save downloaded images %s", enabled ? "enabled" : "disabled");
}

bool config_manager_get_save_downloaded_images(void)
{
    return save_downloaded_images;
}

void config_manager_set_image_etag(const char *etag)
{
    const char *new_etag = etag ? etag : "";

    // No-op if value unchanged — avoids NVS wear when the server does not send
    // an ETag (empty stays empty across every 200) or repeats the same ETag.
    if (strncmp(image_etag, new_etag, HTTP_ETAG_MAX_LEN) == 0) {
        return;
    }

    strncpy(image_etag, new_etag, HTTP_ETAG_MAX_LEN - 1);
    image_etag[HTTP_ETAG_MAX_LEN - 1] = '\0';

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        if (image_etag[0] != '\0') {
            nvs_set_str(nvs_handle, NVS_IMAGE_ETAG_KEY, image_etag);
        } else {
            nvs_erase_key(nvs_handle, NVS_IMAGE_ETAG_KEY);
        }
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }
}

const char *config_manager_get_image_etag(void)
{
    return image_etag;
}
// ============================================================================
// Home Assistant
// ============================================================================

void config_manager_set_ha_url(const char *url)
{
    if (url) {
        strncpy(ha_url, url, HA_URL_MAX_LEN - 1);
        ha_url[HA_URL_MAX_LEN - 1] = '\0';

        // Strip trailing slashes so callers can safely append "/api/...".
        // A doubled slash ("host//api/...") is a different path to Home
        // Assistant's router and returns 404.
        size_t len = strlen(ha_url);
        while (len > 0 && ha_url[len - 1] == '/') {
            ha_url[--len] = '\0';
        }

        nvs_handle_t nvs_handle;
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
            nvs_set_str(nvs_handle, NVS_HA_URL_KEY, ha_url);
            nvs_commit(nvs_handle);
            nvs_close(nvs_handle);
        }

        ESP_LOGI(TAG, "HA URL set to: %s", ha_url);
    }
}
const char *config_manager_get_ha_url(void)
{
    return ha_url;
}

void config_manager_set_ha_enabled(bool enabled)
{
    ha_enabled = enabled;

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_u8(nvs_handle, NVS_HA_ENABLED_KEY, enabled ? 1 : 0);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Home Assistant integration %s", enabled ? "enabled" : "disabled");
}

bool config_manager_get_ha_enabled(void)
{
    return ha_enabled;
}

// ============================================================================
// Telegram Bot
// ============================================================================

void config_manager_set_telegram_bot_token(const char *token)
{
    const char *new_token = token ? token : "";
    strncpy(telegram_bot_token, new_token, TELEGRAM_BOT_TOKEN_MAX_LEN - 1);
    telegram_bot_token[TELEGRAM_BOT_TOKEN_MAX_LEN - 1] = '\0';

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        if (telegram_bot_token[0] != '\0') {
            nvs_set_str(nvs_handle, NVS_TELEGRAM_BOT_TOKEN_KEY, telegram_bot_token);
        } else {
            nvs_erase_key(nvs_handle, NVS_TELEGRAM_BOT_TOKEN_KEY);
        }
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Telegram bot token %s", telegram_bot_token[0] ? "set" : "cleared");
}

const char *config_manager_get_telegram_bot_token(void)
{
    return telegram_bot_token;
}

void config_manager_set_telegram_chat_id(const char *chat_id)
{
    const char *new_id = chat_id ? chat_id : "";
    strncpy(telegram_chat_id, new_id, TELEGRAM_CHAT_ID_MAX_LEN - 1);
    telegram_chat_id[TELEGRAM_CHAT_ID_MAX_LEN - 1] = '\0';

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        if (telegram_chat_id[0] != '\0') {
            nvs_set_str(nvs_handle, NVS_TELEGRAM_CHAT_ID_KEY, telegram_chat_id);
        } else {
            nvs_erase_key(nvs_handle, NVS_TELEGRAM_CHAT_ID_KEY);
        }
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Telegram chat ID %s", telegram_chat_id[0] ? "set" : "cleared");
}

const char *config_manager_get_telegram_chat_id(void)
{
    return telegram_chat_id;
}

bool config_manager_telegram_is_configured(void)
{
    return telegram_bot_token[0] != '\0' && telegram_chat_id[0] != '\0';
}

void config_manager_set_telegram_last_update_id(int64_t update_id)
{
    telegram_last_update_id = update_id;

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_i64(nvs_handle, NVS_TELEGRAM_LAST_UPDATE_ID_KEY, telegram_last_update_id);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Telegram last update_id set to: %lld", (long long) telegram_last_update_id);
}

int64_t config_manager_get_telegram_last_update_id(void)
{
    return telegram_last_update_id;
}

void config_manager_set_telegram_pairing_enabled(bool enabled)
{
    telegram_pairing_enabled = enabled;

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_u8(nvs_handle, NVS_TELEGRAM_PAIRING_KEY, enabled ? 1 : 0);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Telegram orientation pairing %s", enabled ? "enabled" : "disabled");
}

bool config_manager_get_telegram_pairing_enabled(void)
{
    return telegram_pairing_enabled;
}

void config_manager_add_telegram_pending_image(const char *path, const char *caption)
{
    if (!path || path[0] == '\0') {
        return;
    }
    if (telegram_pending_image_count >= TELEGRAM_MAX_PENDING_IMAGES) {
        ESP_LOGW(TAG,
                 "Telegram pending-pair queue full (%d), cannot track %s (file is still saved on "
                 "storage)",
                 TELEGRAM_MAX_PENDING_IMAGES, path);
        return;
    }

    telegram_pending_image_t *e = &telegram_pending_images[telegram_pending_image_count++];
    strncpy(e->path, path, sizeof(e->path) - 1);
    e->path[sizeof(e->path) - 1] = '\0';
    strncpy(e->caption, caption ? caption : "", sizeof(e->caption) - 1);
    e->caption[sizeof(e->caption) - 1] = '\0';

    telegram_pending_persist();
    ESP_LOGI(TAG, "Queued %s for orientation pairing (%d pending)", path,
             telegram_pending_image_count);
}

int config_manager_get_telegram_pending_image_count(void)
{
    return telegram_pending_image_count;
}

bool config_manager_get_telegram_pending_image_at(int index, char *path_out, size_t path_out_len,
                                                  char *caption_out, size_t caption_out_len)
{
    if (index < 0 || index >= telegram_pending_image_count) {
        return false;
    }
    if (path_out) {
        snprintf(path_out, path_out_len, "%s", telegram_pending_images[index].path);
    }
    if (caption_out) {
        snprintf(caption_out, caption_out_len, "%s", telegram_pending_images[index].caption);
    }
    return true;
}

void config_manager_remove_telegram_pending_image_at(int index)
{
    if (index < 0 || index >= telegram_pending_image_count) {
        return;
    }
    for (int i = index; i < telegram_pending_image_count - 1; i++) {
        telegram_pending_images[i] = telegram_pending_images[i + 1];
    }
    telegram_pending_image_count--;
    telegram_pending_persist();
}

void config_manager_clear_telegram_pending_images(void)
{
    telegram_pending_image_count = 0;
    telegram_pending_persist();
    ESP_LOGI(TAG, "Cleared Telegram pending-pair queue (files were left on storage)");
}

void config_manager_set_telegram_low_battery_warned(bool warned)
{
    telegram_low_battery_warned = warned;

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_u8(nvs_handle, NVS_TELEGRAM_LOW_BATT_WARNED_KEY, warned ? 1 : 0);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }
}

bool config_manager_get_telegram_low_battery_warned(void)
{
    return telegram_low_battery_warned;
}

void config_manager_set_telegram_wake_notify_enabled(bool enabled)
{
    telegram_wake_notify_enabled = enabled;

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_u8(nvs_handle, NVS_TELEGRAM_WAKE_NOTIFY_KEY, enabled ? 1 : 0);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Telegram wake-up notification %s", enabled ? "enabled" : "disabled");
}

bool config_manager_get_telegram_wake_notify_enabled(void)
{
    return telegram_wake_notify_enabled;
}

// ============================================================================
// Error overlay / WiFi failure tracking
// ============================================================================

void config_manager_set_error_overlay_enabled(bool enabled)
{
    error_overlay_enabled = enabled;

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_u8(nvs_handle, NVS_ERROR_OVERLAY_ENABLED_KEY, enabled ? 1 : 0);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Critical-error display overlay %s", enabled ? "enabled" : "disabled");
}

bool config_manager_get_error_overlay_enabled(void)
{
    return error_overlay_enabled;
}

void config_manager_set_wifi_fail_count(int count)
{
    wifi_fail_count = count;

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_i32(nvs_handle, NVS_WIFI_FAIL_COUNT_KEY, (int32_t) wifi_fail_count);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }
}

int config_manager_get_wifi_fail_count(void)
{
    return wifi_fail_count;
}

// ============================================================================
// WiFi
// ============================================================================

void config_manager_set_wifi_performance_mode_enabled(bool enabled)
{
    wifi_performance_mode_enabled = enabled;

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_u8(nvs_handle, NVS_WIFI_PERF_MODE_ENABLED_KEY, enabled ? 1 : 0);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "WiFi performance mode %s", enabled ? "enabled" : "disabled");
}

bool config_manager_get_wifi_performance_mode_enabled(void)
{
    return wifi_performance_mode_enabled;
}

void config_manager_set_rotation_pairing_enabled(bool enabled)
{
    rotation_pairing_enabled = enabled;

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_u8(nvs_handle, NVS_ROTATION_PAIRING_ENABLED_KEY, enabled ? 1 : 0);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Auto-rotate orientation pairing %s", enabled ? "enabled" : "disabled");
}

bool config_manager_get_rotation_pairing_enabled(void)
{
    return rotation_pairing_enabled;
}

void config_manager_set_telegram_rotation_notify_enabled(bool enabled)
{
    telegram_rotation_notify_enabled = enabled;

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_u8(nvs_handle, NVS_TELEGRAM_ROTATION_NOTIFY_KEY, enabled ? 1 : 0);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Telegram fallback-rotation notification %s", enabled ? "enabled" : "disabled");
}

bool config_manager_get_telegram_rotation_notify_enabled(void)
{
    return telegram_rotation_notify_enabled;
}

// ============================================================================
// OTA
// ============================================================================

void config_manager_set_ota_check_enabled(bool enabled)
{
    ota_check_enabled = enabled;

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_u8(nvs_handle, NVS_OTA_CHECK_ENABLED_KEY, enabled ? 1 : 0);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Automatic OTA check %s", enabled ? "enabled" : "disabled");
}

bool config_manager_get_ota_check_enabled(void)
{
    return ota_check_enabled;
}

// ============================================================================
// AI Generation
// ============================================================================

void config_manager_set_openai_api_key(const char *key)
{
    if (key == NULL) {
        return;
    }

    strncpy(openai_api_key, key, AI_API_KEY_MAX_LEN - 1);
    openai_api_key[AI_API_KEY_MAX_LEN - 1] = '\0';

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_str(nvs_handle, NVS_OPENAI_API_KEY_KEY, openai_api_key);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "OpenAI API Key set");
}

const char *config_manager_get_openai_api_key(void)
{
    return openai_api_key;
}

void config_manager_set_google_api_key(const char *key)
{
    if (key == NULL) {
        return;
    }

    strncpy(google_api_key, key, AI_API_KEY_MAX_LEN - 1);
    google_api_key[AI_API_KEY_MAX_LEN - 1] = '\0';

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_str(nvs_handle, NVS_GOOGLE_API_KEY_KEY, google_api_key);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Google API Key set");
}

const char *config_manager_get_google_api_key(void)
{
    return google_api_key;
}

void config_manager_set_deep_sleep_enabled(bool enabled)
{
    deep_sleep_enabled = enabled;

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_u8(nvs_handle, NVS_DEEP_SLEEP_KEY, enabled ? 1 : 0);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Deep sleep %s", enabled ? "enabled" : "disabled");
}

bool config_manager_get_deep_sleep_enabled(void)
{
    return deep_sleep_enabled;
}

void config_manager_set_debug_log_enabled(bool enabled)
{
    debug_log_enabled = enabled;

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_u8(nvs_handle, NVS_DEBUG_LOG_KEY, enabled ? 1 : 0);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Debug log %s", enabled ? "enabled" : "disabled");
}

bool config_manager_get_debug_log_enabled(void)
{
    return debug_log_enabled;
}

void config_manager_set_config_last_updated(int64_t timestamp)
{
    config_last_updated = timestamp;
    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_i64(nvs_handle, "cfg_updated", config_last_updated);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }
}

int64_t config_manager_get_config_last_updated(void)
{
    return config_last_updated;
}

void config_manager_touch_config(void)
{
    time_t now;
    time(&now);
    config_manager_set_config_last_updated((int64_t) now);
}
