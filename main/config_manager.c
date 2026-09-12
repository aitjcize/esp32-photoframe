#include "config_manager.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "board_hal.h"
#include "chime.h"
#include "chime_name.h"
#include "config.h"
#include "esp_log.h"
#include "nvs.h"
#include "settings_backup.h"
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

// AI API Keys
static char openai_api_key[AI_API_KEY_MAX_LEN] = {0};
static char google_api_key[AI_API_KEY_MAX_LEN] = {0};

// Power
static bool deep_sleep_enabled = true;  // Enabled by default
static bool chime_enabled = true;       // Speaker chime on display (default on)
static char chime_preset[CHIME_PRESET_MAX_LEN] = DEFAULT_CHIME_PRESET;
static char chime_url[IMAGE_URL_MAX_LEN] = {0};
static chime_source_t chime_source = CHIME_SOURCE_PRESET;
static chime_pull_mode_t chime_pull_mode = CHIME_PULL_WITH_ROTATE;
static chime_play_when_t chime_play_when = CHIME_PLAY_WHEN_AFTER;
static char chime_file[CHIME_FILENAME_MAX_LEN] = {0};

// Debugging
static bool debug_log_enabled = false;

// Config sync
static int64_t config_last_updated = 0;

// True when NVS already holds at least one key that the SD settings snapshot
// covers. After a merged-bin flash at 0x0 this stays false so the SD backup
// can be imported.
static bool nvs_has_settings_backup_keys = false;

static void note_nvs_backup_key(void)
{
    nvs_has_settings_backup_keys = true;
}

static bool settings_sd_available(void)
{
    return storage_get_type() == STORAGE_TYPE_SDCARD;
}

static bool settings_sd_file_present(void)
{
    struct stat st;
    return settings_sd_available() && stat(SETTINGS_BACKUP_PATH, &st) == 0 && st.st_size > 0;
}

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
            note_nvs_backup_key();
            ESP_LOGI(TAG, "Loaded auto-rotate enabled from NVS: %s",
                     auto_rotate_enabled ? "yes" : "no");
        }

        // Rotation schedule (cron). If absent, fall back to migrating a legacy
        // interval, else seed the default — both handled after this handle closes.
        size_t cron_len = sizeof(cron_buf);
        if (nvs_get_str(nvs_handle, NVS_ROTATE_CRON_KEY, cron_buf, &cron_len) == ESP_OK) {
            cron_load_from_joined(cron_buf);
            seed_default_cron = false;
            note_nvs_backup_key();
            ESP_LOGI(TAG, "Loaded %d cron rule(s) from NVS", cron_rule_count);
        } else if (nvs_get_i32(nvs_handle, NVS_ROTATE_INTERVAL_KEY, &legacy_interval) == ESP_OK) {
            migrate_legacy_interval = true;
            seed_default_cron = false;
        }

        uint8_t stored_mode = ROTATION_MODE_URL;  // Default fallback
        if (nvs_get_u8(nvs_handle, NVS_ROTATION_MODE_KEY, &stored_mode) == ESP_OK) {
            rotation_mode = (rotation_mode_t) stored_mode;
            note_nvs_backup_key();
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
            note_nvs_backup_key();
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
            note_nvs_backup_key();
            ESP_LOGI(TAG, "Loaded deep sleep setting from NVS: %s",
                     deep_sleep_enabled ? "enabled" : "disabled");
        }

        uint8_t chime_val = 1;  // Default on
        if (nvs_get_u8(nvs_handle, NVS_CHIME_ENABLED_KEY, &chime_val) == ESP_OK) {
            chime_enabled = (chime_val != 0);
            note_nvs_backup_key();
            ESP_LOGI(TAG, "Loaded speaker chime setting from NVS: %s",
                     chime_enabled ? "enabled" : "disabled");
        }

        size_t chime_preset_len = sizeof(chime_preset);
        if (nvs_get_str(nvs_handle, NVS_CHIME_PRESET_KEY, chime_preset, &chime_preset_len) ==
            ESP_OK) {
            note_nvs_backup_key();
            if (chime_preset_is_valid(chime_preset)) {
                ESP_LOGI(TAG, "Loaded chime preset from NVS: %s", chime_preset);
            } else {
                strncpy(chime_preset, DEFAULT_CHIME_PRESET, CHIME_PRESET_MAX_LEN - 1);
                chime_preset[CHIME_PRESET_MAX_LEN - 1] = '\0';
            }
        } else {
            strncpy(chime_preset, DEFAULT_CHIME_PRESET, CHIME_PRESET_MAX_LEN - 1);
            chime_preset[CHIME_PRESET_MAX_LEN - 1] = '\0';
        }

        size_t chime_url_len = sizeof(chime_url);
        if (nvs_get_str(nvs_handle, NVS_CHIME_URL_KEY, chime_url, &chime_url_len) == ESP_OK) {
            note_nvs_backup_key();
            ESP_LOGI(TAG, "Loaded chime URL from NVS: %s", chime_url);
        }

        uint8_t chime_source_val = CHIME_SOURCE_PRESET;
        if (nvs_get_u8(nvs_handle, NVS_CHIME_SOURCE_KEY, &chime_source_val) == ESP_OK) {
            note_nvs_backup_key();
            if (chime_source_val == CHIME_SOURCE_PRESET || chime_source_val == CHIME_SOURCE_WAV ||
                chime_source_val == CHIME_SOURCE_UPLOADED) {
                chime_source = (chime_source_t) chime_source_val;
            }
        }

        size_t chime_file_len = sizeof(chime_file);
        if (nvs_get_str(nvs_handle, NVS_CHIME_FILE_KEY, chime_file, &chime_file_len) == ESP_OK) {
            note_nvs_backup_key();
            if (chime_filename_is_valid(chime_file)) {
                ESP_LOGI(TAG, "Loaded chime file from NVS: %s", chime_file);
            } else {
                chime_file[0] = '\0';
            }
        } else {
            chime_file[0] = '\0';
        }

        uint8_t chime_pull_val = CHIME_PULL_WITH_ROTATE;
        if (nvs_get_u8(nvs_handle, NVS_CHIME_PULL_MODE_KEY, &chime_pull_val) == ESP_OK) {
            note_nvs_backup_key();
            if (chime_pull_val == CHIME_PULL_ONCE || chime_pull_val == CHIME_PULL_WITH_ROTATE) {
                chime_pull_mode = (chime_pull_mode_t) chime_pull_val;
            }
        }

        uint8_t chime_when_val = CHIME_PLAY_WHEN_AFTER;
        if (nvs_get_u8(nvs_handle, NVS_CHIME_PLAY_WHEN_KEY, &chime_when_val) == ESP_OK) {
            note_nvs_backup_key();
            if (chime_when_val == CHIME_PLAY_WHEN_AFTER ||
                chime_when_val == CHIME_PLAY_WHEN_BEFORE) {
                chime_play_when = (chime_play_when_t) chime_when_val;
            }
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
            note_nvs_backup_key();
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

    // After a full flash at 0x0, NVS is empty but the SD snapshot from the
    // last Settings save is still on the card. Import it before any rotate
    // or UI path reads the runtime config.
    if (settings_backup_should_restore(nvs_has_settings_backup_keys, settings_sd_file_present())) {
        ESP_LOGI(TAG, "NVS looks factory-fresh; restoring settings from %s", SETTINGS_BACKUP_PATH);
        if (config_manager_import_settings_sd() == ESP_OK) {
            nvs_has_settings_backup_keys = true;
            config_manager_touch_config();
        }
    }

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

void config_manager_set_chime_enabled(bool enabled)
{
    chime_enabled = enabled;

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_u8(nvs_handle, NVS_CHIME_ENABLED_KEY, enabled ? 1 : 0);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Speaker chime %s", enabled ? "enabled" : "disabled");
}

bool config_manager_get_chime_enabled(void)
{
    return chime_enabled;
}

static void nvs_store_str_or_erase(const char *key, const char *value)
{
    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) != ESP_OK) {
        return;
    }
    if (value && value[0] != '\0') {
        nvs_set_str(nvs_handle, key, value);
    } else {
        nvs_erase_key(nvs_handle, key);
    }
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
}

void config_manager_set_chime_preset(const char *preset)
{
    const char *value = (preset && preset[0]) ? preset : DEFAULT_CHIME_PRESET;
    strncpy(chime_preset, value, CHIME_PRESET_MAX_LEN - 1);
    chime_preset[CHIME_PRESET_MAX_LEN - 1] = '\0';
    nvs_store_str_or_erase(NVS_CHIME_PRESET_KEY, chime_preset);
    ESP_LOGI(TAG, "Chime preset set to: %s", chime_preset);
}

const char *config_manager_get_chime_preset(void)
{
    return chime_preset[0] ? chime_preset : DEFAULT_CHIME_PRESET;
}

void config_manager_set_chime_url(const char *url)
{
    const char *new_url = url ? url : "";
    bool url_changed = strcmp(chime_url, new_url) != 0;

    strncpy(chime_url, new_url, IMAGE_URL_MAX_LEN - 1);
    chime_url[IMAGE_URL_MAX_LEN - 1] = '\0';
    nvs_store_str_or_erase(NVS_CHIME_URL_KEY, chime_url);

    if (url_changed) {
        unlink(CHIME_CACHE_PATH);
        unlink(CHIME_CACHE_TMP_PATH);
    }

    ESP_LOGI(TAG, "Chime URL set to: %s", chime_url[0] ? chime_url : "(empty)");
}

const char *config_manager_get_chime_url(void)
{
    return chime_url;
}

void config_manager_set_chime_source(chime_source_t source)
{
    if (source != CHIME_SOURCE_PRESET && source != CHIME_SOURCE_WAV &&
        source != CHIME_SOURCE_UPLOADED) {
        source = CHIME_SOURCE_PRESET;
    }
    chime_source = source;

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_u8(nvs_handle, NVS_CHIME_SOURCE_KEY, (uint8_t) source);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    const char *name = "preset";
    if (source == CHIME_SOURCE_WAV) {
        name = "wav";
    } else if (source == CHIME_SOURCE_UPLOADED) {
        name = "uploaded";
    }
    ESP_LOGI(TAG, "Chime source set to: %s", name);
}

chime_source_t config_manager_get_chime_source(void)
{
    return chime_source;
}

void config_manager_set_chime_pull_mode(chime_pull_mode_t mode)
{
    if (mode != CHIME_PULL_ONCE && mode != CHIME_PULL_WITH_ROTATE) {
        mode = CHIME_PULL_WITH_ROTATE;
    }
    chime_pull_mode = mode;

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_u8(nvs_handle, NVS_CHIME_PULL_MODE_KEY, (uint8_t) mode);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Chime pull mode set to: %s",
             mode == CHIME_PULL_WITH_ROTATE ? "with_rotate" : "once");
}

chime_pull_mode_t config_manager_get_chime_pull_mode(void)
{
    return chime_pull_mode;
}

void config_manager_set_chime_play_when(chime_play_when_t when)
{
    if (when != CHIME_PLAY_WHEN_AFTER && when != CHIME_PLAY_WHEN_BEFORE) {
        when = CHIME_PLAY_WHEN_AFTER;
    }
    chime_play_when = when;

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_u8(nvs_handle, NVS_CHIME_PLAY_WHEN_KEY, (uint8_t) when);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Chime play when set to: %s",
             when == CHIME_PLAY_WHEN_BEFORE ? "before" : "after");
}

chime_play_when_t config_manager_get_chime_play_when(void)
{
    return chime_play_when;
}

void config_manager_set_chime_file(const char *filename)
{
    const char *value = filename ? filename : "";
    if (value[0] && !chime_filename_is_valid(value)) {
        ESP_LOGW(TAG, "Ignoring invalid chime filename: %s", value);
        return;
    }
    strncpy(chime_file, value, CHIME_FILENAME_MAX_LEN - 1);
    chime_file[CHIME_FILENAME_MAX_LEN - 1] = '\0';
    nvs_store_str_or_erase(NVS_CHIME_FILE_KEY, chime_file);
    ESP_LOGI(TAG, "Chime file set to: %s", chime_file[0] ? chime_file : "(none)");
}

const char *config_manager_get_chime_file(void)
{
    return chime_file;
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
    (void) config_manager_export_settings_sd();
}

static void settings_backup_from_runtime(settings_backup_t *out)
{
    memset(out, 0, sizeof(*out));
    out->auto_rotate = auto_rotate_enabled;
    out->has_auto_rotate = true;
    out->rotate_cron_count = cron_rule_count;
    if (out->rotate_cron_count > SETTINGS_BACKUP_MAX_CRON_RULES) {
        out->rotate_cron_count = SETTINGS_BACKUP_MAX_CRON_RULES;
    }
    for (int i = 0; i < out->rotate_cron_count; i++) {
        strncpy(out->rotate_cron[i], cron_rules_store[i], SETTINGS_BACKUP_CRON_RULE_MAX_LEN - 1);
        out->rotate_cron[i][SETTINGS_BACKUP_CRON_RULE_MAX_LEN - 1] = '\0';
    }
    out->has_rotate_cron = out->rotate_cron_count > 0;
    strncpy(out->rotation_mode, rotation_mode == ROTATION_MODE_URL ? "url" : "storage",
            sizeof(out->rotation_mode) - 1);
    out->has_rotation_mode = true;
    strncpy(out->image_url, image_url, sizeof(out->image_url) - 1);
    out->has_image_url = true;
    out->deep_sleep_enabled = deep_sleep_enabled;
    out->has_deep_sleep_enabled = true;
    out->chime_enabled = chime_enabled;
    out->has_chime_enabled = true;
    strncpy(out->chime_preset, chime_preset[0] ? chime_preset : DEFAULT_CHIME_PRESET,
            sizeof(out->chime_preset) - 1);
    out->has_chime_preset = true;
    strncpy(out->chime_url, chime_url, sizeof(out->chime_url) - 1);
    out->has_chime_url = true;
    const char *src = "preset";
    if (chime_source == CHIME_SOURCE_WAV) {
        src = "wav";
    } else if (chime_source == CHIME_SOURCE_UPLOADED) {
        src = "uploaded";
    }
    strncpy(out->chime_source, src, sizeof(out->chime_source) - 1);
    out->has_chime_source = true;
    strncpy(out->chime_pull_mode,
            chime_pull_mode == CHIME_PULL_WITH_ROTATE ? "with_rotate" : "once",
            sizeof(out->chime_pull_mode) - 1);
    out->has_chime_pull_mode = true;
    strncpy(out->chime_play_when, chime_play_when == CHIME_PLAY_WHEN_BEFORE ? "before" : "after",
            sizeof(out->chime_play_when) - 1);
    out->has_chime_play_when = true;
    strncpy(out->chime_file, chime_file, sizeof(out->chime_file) - 1);
    out->has_chime_file = true;
}

static void set_chime_url_keep_cache(const char *url)
{
    const char *new_url = url ? url : "";
    strncpy(chime_url, new_url, IMAGE_URL_MAX_LEN - 1);
    chime_url[IMAGE_URL_MAX_LEN - 1] = '\0';
    nvs_store_str_or_erase(NVS_CHIME_URL_KEY, chime_url);
    ESP_LOGI(TAG, "Chime URL restored to: %s", chime_url[0] ? chime_url : "(empty)");
}

static void settings_backup_apply_to_runtime(const settings_backup_t *in)
{
    if (in->has_auto_rotate) {
        config_manager_set_auto_rotate(in->auto_rotate);
    }
    if (in->has_rotate_cron && in->rotate_cron_count > 0) {
        const char *rules[MAX_CRON_RULES];
        int n = 0;
        int count = in->rotate_cron_count;
        if (count > MAX_CRON_RULES) {
            count = MAX_CRON_RULES;
        }
        for (int i = 0; i < count; i++) {
            rules[n++] = in->rotate_cron[i];
        }
        config_manager_set_cron_rules(rules, n);
    }
    if (in->has_rotation_mode) {
        rotation_mode_t mode = ROTATION_MODE_STORAGE;
        if (strcmp(in->rotation_mode, "url") == 0) {
            mode = ROTATION_MODE_URL;
        }
        config_manager_set_rotation_mode(mode);
    }
    if (in->has_image_url) {
        config_manager_set_image_url(in->image_url);
    }
    if (in->has_deep_sleep_enabled) {
        config_manager_set_deep_sleep_enabled(in->deep_sleep_enabled);
    }
    if (in->has_chime_enabled) {
        config_manager_set_chime_enabled(in->chime_enabled);
    }
    if (in->has_chime_preset) {
        config_manager_set_chime_preset(in->chime_preset);
    }
    if (in->has_chime_url) {
        // Do not unlink the SD WAV cache: after a reflash the URL is "new"
        // in empty NVS but the cached file on the card is still valid.
        set_chime_url_keep_cache(in->chime_url);
    }
    if (in->has_chime_source) {
        chime_source_t source = CHIME_SOURCE_PRESET;
        if (strcmp(in->chime_source, "wav") == 0) {
            source = CHIME_SOURCE_WAV;
        } else if (strcmp(in->chime_source, "uploaded") == 0) {
            source = CHIME_SOURCE_UPLOADED;
        }
        config_manager_set_chime_source(source);
    }
    if (in->has_chime_pull_mode) {
        config_manager_set_chime_pull_mode(strcmp(in->chime_pull_mode, "with_rotate") == 0
                                               ? CHIME_PULL_WITH_ROTATE
                                               : CHIME_PULL_ONCE);
    }
    if (in->has_chime_play_when) {
        config_manager_set_chime_play_when(strcmp(in->chime_play_when, "before") == 0
                                               ? CHIME_PLAY_WHEN_BEFORE
                                               : CHIME_PLAY_WHEN_AFTER);
    }
    if (in->has_chime_file) {
        config_manager_set_chime_file(in->chime_file);
    }
}

esp_err_t config_manager_export_settings_sd(void)
{
    if (!settings_sd_available()) {
        return ESP_ERR_NOT_FOUND;
    }

    settings_backup_t snap;
    settings_backup_from_runtime(&snap);

    char buf[4096];
    if (settings_backup_serialize(&snap, buf, sizeof(buf)) < 0) {
        ESP_LOGW(TAG, "Failed to serialize settings snapshot");
        return ESP_FAIL;
    }

    if (mkdir(SETTINGS_BACKUP_DIR, 0775) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "Failed to create %s: errno %d", SETTINGS_BACKUP_DIR, errno);
        return ESP_FAIL;
    }

    FILE *f = fopen(SETTINGS_BACKUP_TMP_PATH, "w");
    if (!f) {
        ESP_LOGW(TAG, "Failed to open %s for write", SETTINGS_BACKUP_TMP_PATH);
        return ESP_FAIL;
    }
    size_t len = strlen(buf);
    size_t written = fwrite(buf, 1, len, f);
    int flush_err = fflush(f);
    fclose(f);
    if (written != len || flush_err != 0) {
        unlink(SETTINGS_BACKUP_TMP_PATH);
        ESP_LOGW(TAG, "Failed to write settings snapshot");
        return ESP_FAIL;
    }
    unlink(SETTINGS_BACKUP_PATH);
    if (rename(SETTINGS_BACKUP_TMP_PATH, SETTINGS_BACKUP_PATH) != 0) {
        unlink(SETTINGS_BACKUP_TMP_PATH);
        ESP_LOGW(TAG, "Failed to replace %s", SETTINGS_BACKUP_PATH);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Wrote settings snapshot to %s", SETTINGS_BACKUP_PATH);
    return ESP_OK;
}

esp_err_t config_manager_import_settings_sd(void)
{
    if (!settings_sd_file_present()) {
        return ESP_ERR_NOT_FOUND;
    }

    FILE *f = fopen(SETTINGS_BACKUP_PATH, "r");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return ESP_FAIL;
    }
    long sz = ftell(f);
    if (sz <= 0 || sz > 8192) {
        fclose(f);
        ESP_LOGW(TAG, "Settings snapshot has invalid size %ld", sz);
        return ESP_ERR_INVALID_SIZE;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return ESP_FAIL;
    }

    char *buf = malloc((size_t) sz + 1);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t n = fread(buf, 1, (size_t) sz, f);
    fclose(f);
    buf[n] = '\0';

    settings_backup_t snap;
    bool parsed = settings_backup_parse(buf, &snap);
    free(buf);
    if (!parsed) {
        ESP_LOGW(TAG, "Failed to parse %s", SETTINGS_BACKUP_PATH);
        return ESP_ERR_INVALID_ARG;
    }

    settings_backup_apply_to_runtime(&snap);
    ESP_LOGI(TAG, "Imported settings snapshot from %s", SETTINGS_BACKUP_PATH);
    return ESP_OK;
}

void config_manager_delete_settings_sd(void)
{
    if (!settings_sd_available()) {
        return;
    }
    unlink(SETTINGS_BACKUP_PATH);
    unlink(SETTINGS_BACKUP_TMP_PATH);
}
