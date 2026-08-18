#include "utils.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "board_hal.h"
#include "cJSON.h"
#include "cert_pin.h"
#include "color_palette.h"
#include "config.h"
#include "config_manager.h"
#include "cron.h"
#include "debug_log.h"
#include "display_flow.h"
#include "display_manager.h"
#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "image_processor.h"
#include "mdns_service.h"
#include "nvs.h"
#include "periodic_tasks.h"
#include "power_manager.h"
#include "processing_settings.h"
#include "storage.h"
#include "telegram_bot.h"
#include "wifi_manager.h"

static const char *TAG = "utils";

// Last image fetch error, shown on the auto-rotate UI. Persisted to NVS so it
// survives deep sleep — a fetch fails right before the device sleeps again, and
// the in-memory copy would otherwise be lost by the next boot.
static char last_fetch_error[256] = {0};
static bool last_fetch_error_loaded = false;

static void last_fetch_error_load(void)
{
    if (last_fetch_error_loaded) {
        return;
    }
    last_fetch_error_loaded = true;

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle) == ESP_OK) {
        size_t len = sizeof(last_fetch_error);
        if (nvs_get_str(nvs_handle, NVS_LAST_FETCH_ERROR_KEY, last_fetch_error, &len) != ESP_OK) {
            last_fetch_error[0] = '\0';
        }
        nvs_close(nvs_handle);
    }
}

void utils_set_last_fetch_error(const char *error)
{
    last_fetch_error_load();  // make sure the current value is known before diffing

    char next[sizeof(last_fetch_error)];
    if (error) {
        strncpy(next, error, sizeof(next) - 1);
        next[sizeof(next) - 1] = '\0';
    } else {
        next[0] = '\0';
    }

    if (strcmp(next, last_fetch_error) == 0) {
        return;  // unchanged — avoid a redundant NVS write on every rotation
    }
    strcpy(last_fetch_error, next);

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        if (last_fetch_error[0] != '\0') {
            nvs_set_str(nvs_handle, NVS_LAST_FETCH_ERROR_KEY, last_fetch_error);
        } else {
            nvs_erase_key(nvs_handle, NVS_LAST_FETCH_ERROR_KEY);
        }
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }
}

const char *utils_get_last_fetch_error(void)
{
    last_fetch_error_load();
    return last_fetch_error;
}

// Seconds the server asked us to stay awake after rotating (X-Post-Rotate-Wait-Sec
// on the image response) so it can pull our config. Set by the image fetch's HTTP
// event handler, read by the wake flow. Reset at the start of each fetch.
static int post_rotate_wait_sec = 0;

int utils_get_post_rotate_wait_sec(void)
{
    return post_rotate_wait_sec;
}

// Last cert pin error (transient, consumed by HTTP handler on failure response)
static char last_cert_pin_error[256] = {0};

void utils_set_cert_pin_error(const char *msg)
{
    if (msg) {
        strncpy(last_cert_pin_error, msg, sizeof(last_cert_pin_error) - 1);
        last_cert_pin_error[sizeof(last_cert_pin_error) - 1] = '\0';
    } else {
        last_cert_pin_error[0] = '\0';
    }
}

const char *utils_consume_cert_pin_error(void)
{
    static char out[256];
    strncpy(out, last_cert_pin_error, sizeof(out));
    out[sizeof(out) - 1] = '\0';
    last_cert_pin_error[0] = '\0';
    return out;
}

// Last config-validation error (transient, consumed by HTTP handler on failure)
static char last_config_error[256] = {0};

void utils_set_config_error(const char *msg)
{
    if (msg) {
        ESP_LOGW(TAG, "Config validation failed: %s", msg);
        strncpy(last_config_error, msg, sizeof(last_config_error) - 1);
        last_config_error[sizeof(last_config_error) - 1] = '\0';
    } else {
        last_config_error[0] = '\0';
    }
}

const char *utils_consume_config_error(void)
{
    static char out[256];
    strncpy(out, last_config_error, sizeof(out));
    out[sizeof(out) - 1] = '\0';
    last_config_error[0] = '\0';
    return out;
}

esp_err_t apply_config_from_json(cJSON *root)
{
    cJSON *item;

    // General
    item = cJSON_GetObjectItem(root, "device_name");
    if (item && cJSON_IsString(item)) {
        const char *new_name = cJSON_GetStringValue(item);
        const char *current_name = config_manager_get_device_name();
        if (strcmp(new_name, current_name) != 0) {
            config_manager_set_device_name(new_name);
            mdns_service_update_hostname();
            wifi_manager_update_hostname();
        }
    }

    item = cJSON_GetObjectItem(root, "timezone");
    if (item && cJSON_IsString(item)) {
        const char *tz = cJSON_GetStringValue(item);
        config_manager_set_timezone(tz);
        setenv("TZ", tz, 1);
        tzset();
    }

    // Advanced network settings (#43): custom NTP server, static IP and DNS
    // override. Addresses are validated before persisting so a typo can't
    // strand the frame on an unreachable address; IP settings apply on the
    // next connect (reboot/wake).
    item = cJSON_GetObjectItem(root, "ntp_server");
    if (item && cJSON_IsString(item)) {
        config_manager_set_ntp_server(cJSON_GetStringValue(item));
        periodic_tasks_force_run(SNTP_TASK_NAME);
        periodic_tasks_check_and_run();
    }

    // Clients PATCH only changed fields, so each static address may arrive on
    // its own (edited while already in static mode) or be absent when the
    // request merely flips ip_mode. Store what's present, then a switch to
    // static validates the effective (request-or-stored) values as a set.
    // An empty string is accepted as "not set" (remote sync mirrors back the
    // full config, including blank static fields on a DHCP device); switching
    // to static mode below still validates the effective set.
    esp_ip4_addr_t parsed;
    item = cJSON_GetObjectItem(root, "static_ip");
    if (item && cJSON_IsString(item)) {
        const char *addr = cJSON_GetStringValue(item);
        if (addr[0] != '\0' && esp_netif_str_to_ip4(addr, &parsed) != ESP_OK) {
            utils_set_config_error("Invalid static IP address");
            return ESP_FAIL;
        }
        config_manager_set_static_ip(addr);
    }

    item = cJSON_GetObjectItem(root, "static_netmask");
    if (item && cJSON_IsString(item)) {
        const char *addr = cJSON_GetStringValue(item);
        if (addr[0] != '\0' && esp_netif_str_to_ip4(addr, &parsed) != ESP_OK) {
            utils_set_config_error("Invalid static netmask");
            return ESP_FAIL;
        }
        config_manager_set_static_netmask(addr);
    }

    item = cJSON_GetObjectItem(root, "static_gateway");
    if (item && cJSON_IsString(item)) {
        const char *addr = cJSON_GetStringValue(item);
        if (addr[0] != '\0' && esp_netif_str_to_ip4(addr, &parsed) != ESP_OK) {
            utils_set_config_error("Invalid static gateway");
            return ESP_FAIL;
        }
        config_manager_set_static_gateway(addr);
    }

    item = cJSON_GetObjectItem(root, "ip_mode");
    if (item && cJSON_IsString(item)) {
        bool want_static = (strcmp(cJSON_GetStringValue(item), "static") == 0);
        if (want_static) {
            if (esp_netif_str_to_ip4(config_manager_get_static_ip(), &parsed) != ESP_OK) {
                utils_set_config_error("Invalid static IP address");
                return ESP_FAIL;
            }
            if (esp_netif_str_to_ip4(config_manager_get_static_netmask(), &parsed) != ESP_OK) {
                utils_set_config_error("Invalid static netmask");
                return ESP_FAIL;
            }
            if (esp_netif_str_to_ip4(config_manager_get_static_gateway(), &parsed) != ESP_OK) {
                utils_set_config_error("Invalid static gateway");
                return ESP_FAIL;
            }
            config_manager_set_ip_mode(IP_MODE_STATIC);
        } else {
            config_manager_set_ip_mode(IP_MODE_DHCP);
        }
    }

    item = cJSON_GetObjectItem(root, "dns_server");
    if (item && cJSON_IsString(item)) {
        const char *dns = cJSON_GetStringValue(item);
        if (dns[0] != '\0' && esp_netif_str_to_ip4(dns, &parsed) != ESP_OK) {
            utils_set_config_error("Invalid DNS server address");
            return ESP_FAIL;
        }
        config_manager_set_dns_server(dns);
    }

    // WiFi
    cJSON *wifi_ssid_obj = cJSON_GetObjectItem(root, "wifi_ssid");
    cJSON *wifi_password_obj = cJSON_GetObjectItem(root, "wifi_password");
    if (wifi_ssid_obj && cJSON_IsString(wifi_ssid_obj)) {
        const char *new_ssid = cJSON_GetStringValue(wifi_ssid_obj);
        const char *new_password = NULL;
        if (wifi_password_obj && cJSON_IsString(wifi_password_obj) &&
            strlen(cJSON_GetStringValue(wifi_password_obj)) > 0) {
            new_password = cJSON_GetStringValue(wifi_password_obj);
        }

        const char *current_ssid = config_manager_get_wifi_ssid();
        if (strcmp(new_ssid, current_ssid) != 0 || new_password != NULL) {
            if (new_password == NULL) {
                new_password = config_manager_get_wifi_password();
            }

            ESP_LOGI(TAG, "WiFi credentials changed, testing connection to: %s", new_ssid);

            esp_err_t err = wifi_manager_connect(new_ssid, new_password);
            if (err == ESP_OK) {
                config_manager_set_wifi_ssid(new_ssid);
                if (wifi_password_obj && cJSON_IsString(wifi_password_obj) &&
                    strlen(cJSON_GetStringValue(wifi_password_obj)) > 0) {
                    config_manager_set_wifi_password(new_password);
                }
                ESP_LOGI(TAG, "Successfully connected and saved WiFi credentials");
            } else {
                ESP_LOGW(TAG, "Failed to connect to new WiFi, reverting to previous credentials");
                wifi_manager_connect(current_ssid, config_manager_get_wifi_password());
                return ESP_FAIL;
            }
        }
    }

    item = cJSON_GetObjectItem(root, "display_orientation");
    if (item && cJSON_IsString(item)) {
        const char *orient_str = cJSON_GetStringValue(item);
        if (strcmp(orient_str, "portrait") == 0) {
            config_manager_set_display_orientation(DISPLAY_ORIENTATION_PORTRAIT);
        } else {
            config_manager_set_display_orientation(DISPLAY_ORIENTATION_LANDSCAPE);
        }
    }

    item = cJSON_GetObjectItem(root, "display_rotation_deg");
    if (item && cJSON_IsNumber(item)) {
        int deg = item->valueint;
        // Only 0 and 180 are supported: 90/270 swap Paint's logical
        // dimensions, which the panel-size decode paths and dimensionless
        // .epdgz payloads cannot represent (portrait mounting is handled by
        // display_orientation instead)
        if (deg == 0 || deg == 180) {
            config_manager_set_display_rotation_deg(deg);
            display_manager_initialize_paint();
        } else {
            utils_set_config_error("Display rotation must be 0 or 180 degrees");
            return ESP_FAIL;
        }
    }

    // Auto Rotate
    item = cJSON_GetObjectItem(root, "auto_rotate");
    if (item && cJSON_IsBool(item)) {
        config_manager_set_auto_rotate(cJSON_IsTrue(item));
        power_manager_reset_rotate_timer();
    }

    // Rotation schedule: an array of cron expressions. Validate every rule
    // before applying any; reject the whole request on the first bad one.
    item = cJSON_GetObjectItem(root, "rotate_cron");
    if (item && cJSON_IsArray(item)) {
        int count = cJSON_GetArraySize(item);
        if (count > MAX_CRON_RULES) {
            char msg[64];
            snprintf(msg, sizeof(msg), "Too many schedule rules (max %d)", MAX_CRON_RULES);
            utils_set_config_error(msg);
            return ESP_FAIL;
        }
        // An empty schedule is ambiguous (it would silently fall back to
        // hourly rotation, and the empty set can't be restored after a
        // reboot). Turning auto_rotate off is the way to stop rotating.
        if (count == 0) {
            utils_set_config_error("Schedule must contain at least one rule");
            return ESP_FAIL;
        }
        const char *rules[MAX_CRON_RULES];
        int n = 0;
        cJSON *el;
        cJSON_ArrayForEach(el, item)
        {
            if (!cJSON_IsString(el)) {
                utils_set_config_error("Schedule rule must be a string");
                return ESP_FAIL;
            }
            const char *expr = cJSON_GetStringValue(el);
            if (strlen(expr) >= CRON_RULE_MAX_LEN) {
                utils_set_config_error("Cron expression too long");
                return ESP_FAIL;
            }
            cron_rule_t tmp;
            if (!cron_parse(expr, &tmp)) {
                char msg[96];
                snprintf(msg, sizeof(msg), "Invalid cron expression: %s", expr);
                utils_set_config_error(msg);
                return ESP_FAIL;
            }
            if (n < MAX_CRON_RULES) {
                rules[n++] = expr;
            }
        }
        config_manager_set_cron_rules(rules, n);
        power_manager_reset_rotate_timer();
    } else {
        // Backward compatibility: convert a legacy interval to a cron rule.
        item = cJSON_GetObjectItem(root, "rotate_interval");
        if (item && cJSON_IsNumber(item)) {
            config_manager_set_cron_rules_from_interval(item->valueint);
            power_manager_reset_rotate_timer();
        }
    }

    item = cJSON_GetObjectItem(root, "rotation_mode");
    if (item && cJSON_IsString(item)) {
        const char *mode_str = cJSON_GetStringValue(item);
        rotation_mode_t mode = ROTATION_MODE_STORAGE;
        if (strcmp(mode_str, "url") == 0)
            mode = ROTATION_MODE_URL;
        else if (strcmp(mode_str, "telegram") == 0)
            mode = ROTATION_MODE_TELEGRAM;
        // Backwards compatibility: accept "sdcard" as alias for "storage"
        if (strcmp(mode_str, "sdcard") == 0)
            mode = ROTATION_MODE_STORAGE;
        config_manager_set_rotation_mode(mode);
    }

    // Auto Rotate - SDCARD
    item = cJSON_GetObjectItem(root, "sd_rotation_mode");
    if (item && cJSON_IsString(item)) {
        const char *mode_str = cJSON_GetStringValue(item);
        sd_rotation_mode_t mode =
            (strcmp(mode_str, "sequential") == 0) ? SD_ROTATION_SEQUENTIAL : SD_ROTATION_RANDOM;
        config_manager_set_sd_rotation_mode(mode);
    }

    // Auto Rotate - URL (with auto-pinning)
    item = cJSON_GetObjectItem(root, "image_url");
    if (item && cJSON_IsString(item)) {
        const char *new_url = cJSON_GetStringValue(item);
        const char *cur_url = config_manager_get_image_url();
        if (!cur_url)
            cur_url = "";

        bool new_is_https = (strncmp(new_url, "https://", 8) == 0);
        bool cur_is_https = (strncmp(cur_url, "https://", 8) == 0);
        bool url_changed = (strcmp(new_url, cur_url) != 0);

        if (url_changed) {
            if (new_is_https) {
                char err_buf[256] = {0};
                esp_err_t pin_ret = cert_pin_fetch_and_store(new_url, err_buf, sizeof(err_buf));
                if (pin_ret != ESP_OK) {
                    ESP_LOGE(TAG, "Cert pin failed, rejecting config: %s", err_buf);
                    utils_set_cert_pin_error(err_buf);
                    return ESP_FAIL;
                }
            } else if (cur_is_https) {
                // Downgrading to HTTP/empty: clear the pinned cert
                cert_pin_clear();
            }
            config_manager_set_image_url(new_url);
        }
    }

    item = cJSON_GetObjectItem(root, "access_token");
    if (item && cJSON_IsString(item)) {
        config_manager_set_access_token(cJSON_GetStringValue(item));
    }

    item = cJSON_GetObjectItem(root, "http_header_key");
    if (item && cJSON_IsString(item)) {
        config_manager_set_http_header_key(cJSON_GetStringValue(item));
    }

    item = cJSON_GetObjectItem(root, "http_header_value");
    if (item && cJSON_IsString(item)) {
        config_manager_set_http_header_value(cJSON_GetStringValue(item));
    }

    item = cJSON_GetObjectItem(root, "save_downloaded_images");
    if (item && cJSON_IsBool(item)) {
        config_manager_set_save_downloaded_images(cJSON_IsTrue(item));
    }

    // Home Assistant
    item = cJSON_GetObjectItem(root, "ha_url");
    if (item && cJSON_IsString(item)) {
        config_manager_set_ha_url(cJSON_GetStringValue(item));
    }

    item = cJSON_GetObjectItem(root, "ha_enabled");
    if (item && cJSON_IsBool(item)) {
        config_manager_set_ha_enabled(cJSON_IsTrue(item));
    }

    // Telegram Bot
    item = cJSON_GetObjectItem(root, "telegram_bot_token");
    if (item && cJSON_IsString(item)) {
        config_manager_set_telegram_bot_token(cJSON_GetStringValue(item));
    }

    item = cJSON_GetObjectItem(root, "telegram_chat_id");
    if (item && cJSON_IsString(item)) {
        const char *chat_id = cJSON_GetStringValue(item);
        // Must be empty (clearing) or a plain integer (optionally negative -
        // Telegram uses negative IDs for groups/supergroups).
        bool valid = true;
        for (size_t i = 0; chat_id[i] != '\0' && valid; i++) {
            if (chat_id[i] == '-' && i == 0) {
                continue;
            }
            if (chat_id[i] < '0' || chat_id[i] > '9') {
                valid = false;
            }
        }
        if (!valid) {
            utils_set_config_error("Telegram chat ID must be a numeric ID");
            return ESP_FAIL;
        }
        config_manager_set_telegram_chat_id(chat_id);
    }

    item = cJSON_GetObjectItem(root, "telegram_pairing_enabled");
    if (item && cJSON_IsBool(item)) {
        config_manager_set_telegram_pairing_enabled(cJSON_IsTrue(item));
        if (!cJSON_IsTrue(item)) {
            // Clears the tracking queue only - files stay on storage.
            config_manager_clear_telegram_pending_images();
        }
    }

    item = cJSON_GetObjectItem(root, "telegram_wake_notify_enabled");
    if (item && cJSON_IsBool(item)) {
        config_manager_set_telegram_wake_notify_enabled(cJSON_IsTrue(item));
    }

    // AI API Keys
    item = cJSON_GetObjectItem(root, "openai_api_key");
    if (item && cJSON_IsString(item)) {
        config_manager_set_openai_api_key(cJSON_GetStringValue(item));
    }

    item = cJSON_GetObjectItem(root, "google_api_key");
    if (item && cJSON_IsString(item)) {
        config_manager_set_google_api_key(cJSON_GetStringValue(item));
    }

    // Power
    item = cJSON_GetObjectItem(root, "deep_sleep_enabled");
    if (item && cJSON_IsBool(item)) {
        power_manager_set_deep_sleep_enabled(cJSON_IsTrue(item));
    }

    // Debugging
    item = cJSON_GetObjectItem(root, "debug_log_enabled");
    if (item && cJSON_IsBool(item)) {
        debug_log_set_enabled(cJSON_IsTrue(item));
    }

    // OTA
    item = cJSON_GetObjectItem(root, "ota_check_enabled");
    if (item && cJSON_IsBool(item)) {
        config_manager_set_ota_check_enabled(cJSON_IsTrue(item));
    }

    // Error overlay
    item = cJSON_GetObjectItem(root, "error_overlay_enabled");
    if (item && cJSON_IsBool(item)) {
        config_manager_set_error_overlay_enabled(cJSON_IsTrue(item));
    }

    // WiFi performance mode
    item = cJSON_GetObjectItem(root, "wifi_performance_mode_enabled");
    if (item && cJSON_IsBool(item)) {
        config_manager_set_wifi_performance_mode_enabled(cJSON_IsTrue(item));
    }

    // Auto-rotate orientation pairing (random mode only)
    item = cJSON_GetObjectItem(root, "rotation_pairing_enabled");
    if (item && cJSON_IsBool(item)) {
        config_manager_set_rotation_pairing_enabled(cJSON_IsTrue(item));
    }

    // Telegram notification on fallback-rotation display changes
    item = cJSON_GetObjectItem(root, "telegram_rotation_notify_enabled");
    if (item && cJSON_IsBool(item)) {
        config_manager_set_telegram_rotation_notify_enabled(cJSON_IsTrue(item));
    }

    // Keep a copy of each Telegram photo as received, before e-paper processing
    item = cJSON_GetObjectItem(root, "telegram_keep_originals_enabled");
    if (item && cJSON_IsBool(item)) {
        config_manager_set_telegram_keep_originals_enabled(cJSON_IsTrue(item));
    }

    return ESP_OK;
}

// Context for HTTP event handler
typedef struct {
    FILE *file;
    int total_read;
    char *content_type;
    char *thumbnail_url;   // Optional thumbnail URL from X-Thumbnail-URL header
    char *config_payload;  // Optional config JSON from X-Config-Payload header
    char *etag;            // Optional ETag buffer (HTTP_ETAG_MAX_LEN bytes) for 304 caching
} download_context_t;

// HTTP event handler to write data to file
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    download_context_t *ctx = (download_context_t *) evt->user_data;

    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (ctx->file) {
            fwrite(evt->data, 1, evt->data_len, ctx->file);
            ctx->total_read += evt->data_len;
            // The SD write path busy-polls SPI; on a fast link this handler
            // can run back-to-back for seconds, and together with another
            // busy task it starves the IDLE watchdog. Yield at every 32 KB
            // boundary so the idle task gets a window.
            if ((ctx->total_read >> 15) != ((ctx->total_read - evt->data_len) >> 15)) {
                vTaskDelay(1);
            }
        }
        break;
    case HTTP_EVENT_ON_HEADER:
        if (strcasecmp(evt->header_key, "Content-Type") == 0) {
            snprintf(ctx->content_type, 128, "%s", evt->header_value);
        } else if (strcasecmp(evt->header_key, "X-Thumbnail-URL") == 0) {
            // Capture thumbnail URL if provided by server (case-insensitive)
            if (ctx->thumbnail_url && strlen(evt->header_value) > 0) {
                strncpy(ctx->thumbnail_url, evt->header_value, 511);
                ctx->thumbnail_url[511] = '\0';
                ESP_LOGI(TAG, "Thumbnail URL provided: %s", ctx->thumbnail_url);
            }
        } else if (strcasecmp(evt->header_key, "X-Config-Payload") == 0) {
            // Capture config payload for remote sync
            if (ctx->config_payload && strlen(evt->header_value) > 0) {
                strncpy(ctx->config_payload, evt->header_value, 2047);
                ctx->config_payload[2047] = '\0';
                ESP_LOGI(TAG, "Config payload received from server");
            }
        } else if (strcasecmp(evt->header_key, "X-Post-Rotate-Wait-Sec") == 0) {
            // Server wants us to stay awake after rotating so it can pull our
            // config. Clamp to our own maximum regardless of what it asks for.
            int wait = atoi(evt->header_value);
            if (wait < 0) {
                wait = 0;
            } else if (wait > POST_ROTATE_WAIT_MAX_SEC) {
                wait = POST_ROTATE_WAIT_MAX_SEC;
            }
            post_rotate_wait_sec = wait;
            ESP_LOGI(TAG, "Server requested post-rotate wait: %d s", wait);
        } else if (strcasecmp(evt->header_key, "ETag") == 0) {
            if (ctx->etag) {
                strncpy(ctx->etag, evt->header_value, HTTP_ETAG_MAX_LEN - 1);
                ctx->etag[HTTP_ETAG_MAX_LEN - 1] = '\0';
            }
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

// Download `url` into CURRENT_UPLOAD_PATH with retries. On HTTP 304 sets
// *not_modified and returns ESP_OK with nothing downloaded. On success,
// detects the image format (falling back to the Content-Type header) and
// hands out the optional thumbnail URL and remote-config payload the server
// sent along (heap strings, caller frees; NULL/empty when absent).
static esp_err_t fetch_perform_download(const char *url, bool *not_modified, image_format_t *format,
                                        char **thumbnail_url_out, char **config_payload_out)
{
    // Reset per-fetch; the HTTP event handler sets it if the server sends the
    // X-Post-Rotate-Wait-Sec header (on either a 200 or a 304 response).
    post_rotate_wait_sec = 0;

    const char *temp_upload_path = CURRENT_UPLOAD_PATH;

    esp_err_t err = ESP_FAIL;
    int status_code = 0;
    int content_length = 0;
    char *content_type = NULL;
    char *thumbnail_url_buffer = NULL;
    int total_downloaded = 0;
    const int max_retries = 3;

    char *config_payload_buffer = NULL;
    char *etag_buffer = NULL;

    *thumbnail_url_out = NULL;
    *config_payload_out = NULL;

    // Allocate buffers once before retry loop
    thumbnail_url_buffer = calloc(512, 1);
    content_type = calloc(128, 1);
    config_payload_buffer = calloc(2048, 1);
    etag_buffer = calloc(HTTP_ETAG_MAX_LEN, 1);

    if (!content_type || !thumbnail_url_buffer || !config_payload_buffer || !etag_buffer) {
        ESP_LOGE(TAG, "Failed to allocate memory for download context");
        free(content_type);
        free(thumbnail_url_buffer);
        free(config_payload_buffer);
        free(etag_buffer);
        return ESP_FAIL;
    }

    // Retry loop
    for (int retry = 0; retry < max_retries; retry++) {
        if (retry > 0) {
            ESP_LOGW(TAG, "Retry attempt %d/%d after 3 second delay...", retry + 1, max_retries);
            vTaskDelay(pdMS_TO_TICKS(3000));  // 3 second delay between retries
        }

        FILE *file = fopen(temp_upload_path, "wb");
        if (!file) {
            ESP_LOGE(TAG, "Failed to open file for writing: %s", temp_upload_path);
            continue;  // Try again
        }

        // Clear buffers for this retry
        memset(content_type, 0, 128);
        memset(config_payload_buffer, 0, 2048);
        memset(etag_buffer, 0, HTTP_ETAG_MAX_LEN);

        download_context_t ctx = {.file = file,
                                  .total_read = 0,
                                  .content_type = content_type,
                                  .thumbnail_url = thumbnail_url_buffer,
                                  .config_payload = config_payload_buffer,
                                  .etag = etag_buffer};

        // Use custom CA cert for HTTPS if configured
        size_t pinned_cert_len = 0;
        const uint8_t *pinned_cert = config_manager_get_ca_cert_der(&pinned_cert_len);

        esp_http_client_config_t config = {
            .url = url,
            .timeout_ms = 120000,
            .event_handler = http_event_handler,
            .user_data = &ctx,
            .max_redirection_count = 5,
            .user_agent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36",
            .buffer_size_tx = 2048,
            .cert_der = (const char *) pinned_cert,
            .cert_len = pinned_cert_len,
        };

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) {
            ESP_LOGE(TAG, "Failed to initialize HTTP client");
            fclose(file);
            continue;  // Try again
        }

        // Add Authorization Bearer header if access token is configured
        const char *access_token = config_manager_get_access_token();
        if (access_token && strlen(access_token) > 0) {
            char auth_header[ACCESS_TOKEN_MAX_LEN + 20];  // "Bearer " + token + null terminator
            snprintf(auth_header, sizeof(auth_header), "Bearer %s", access_token);
            esp_http_client_set_header(client, "Authorization", auth_header);
            ESP_LOGI(TAG, "Added Authorization Bearer header (token length: %zu)",
                     strlen(access_token));
        }

        // Add custom HTTP header if configured (will not override Authorization if already set by
        // access token)
        const char *header_key = config_manager_get_http_header_key();
        const char *header_value = config_manager_get_http_header_value();
        if (header_key && strlen(header_key) > 0 && header_value && strlen(header_value) > 0) {
            // Skip if trying to set Authorization header when access token is already set
            if (strcasecmp(header_key, "Authorization") == 0 && access_token &&
                strlen(access_token) > 0) {
                ESP_LOGW(TAG,
                         "Skipping custom Authorization header - access token takes precedence");
            } else {
                esp_http_client_set_header(client, header_key, header_value);
                ESP_LOGI(TAG, "Added custom HTTP header: %s", header_key);
            }
        }

        // Add display resolution and orientation headers
        char width_str[16];
        char height_str[16];
        snprintf(width_str, sizeof(width_str), "%d", BOARD_HAL_DISPLAY_WIDTH);
        snprintf(height_str, sizeof(height_str), "%d", BOARD_HAL_DISPLAY_HEIGHT);
        esp_http_client_set_header(client, "X-Display-Width", width_str);
        esp_http_client_set_header(client, "X-Display-Height", height_str);
        esp_http_client_set_header(
            client, "X-Display-Orientation",
            config_manager_get_display_orientation() == DISPLAY_ORIENTATION_LANDSCAPE ? "landscape"
                                                                                      : "portrait");

        // Add firmware version header
        const esp_app_desc_t *app_desc = esp_app_get_description();
        esp_http_client_set_header(client, "X-Firmware-Version", app_desc->version);

        // Add If-None-Match with stored ETag to enable 304 Not Modified responses.
        // Server may return an opaque ETag header on the previous 200; we echo it
        // back so the server can short-circuit with 304 when content is unchanged.
        const char *stored_etag = config_manager_get_image_etag();
        if (stored_etag && stored_etag[0] != '\0') {
            esp_http_client_set_header(client, "If-None-Match", stored_etag);
        }

        // Add config timestamp for remote sync
        char config_ts[24];
        snprintf(config_ts, sizeof(config_ts), "%lld",
                 (long long) config_manager_get_config_last_updated());
        esp_http_client_set_header(client, "X-Config-Last-Updated", config_ts);

        // Add processing settings as JSON header
        processing_settings_t proc_settings;
        if (processing_settings_load(&proc_settings) != ESP_OK) {
            processing_settings_get_defaults(&proc_settings);
        }
        char *settings_json = processing_settings_to_json(&proc_settings);
        if (settings_json) {
            esp_http_client_set_header(client, "X-Processing-Settings", settings_json);
            free(settings_json);
        }

        // Add color palette as JSON header
        color_palette_t palette;
        if (color_palette_load(&palette) != ESP_OK) {
            color_palette_get_defaults(&palette);
        }
        char *palette_json = color_palette_to_json(&palette);
        if (palette_json) {
            esp_http_client_set_header(client, "X-Color-Palette", palette_json);
            free(palette_json);
        }

        // Add battery level
        char batt_str[4];
        snprintf(batt_str, sizeof(batt_str), "%i", board_hal_get_battery_percent());
        esp_http_client_set_header(client, "X-Battery-Percentage", batt_str);

        err = esp_http_client_perform(client);

        status_code = esp_http_client_get_status_code(client);
        content_length = esp_http_client_get_content_length(client);
        total_downloaded = ctx.total_read;
        content_type = ctx.content_type;

        fclose(file);
        esp_http_client_cleanup(client);

        // 304 Not Modified: server confirmed the cached image is still current.
        // eInk retains the last rendered image without power, so skip the refresh
        // entirely and return early — no download body, no decode, no repaint.
        if (err == ESP_OK && status_code == 304) {
            ESP_LOGI(TAG, "HTTP 304 Not Modified — skipping refresh");
            unlink(temp_upload_path);
            *not_modified = true;
            utils_set_last_fetch_error(NULL);
            free(content_type);
            free(thumbnail_url_buffer);
            free(config_payload_buffer);
            free(etag_buffer);
            return ESP_OK;
        }

        // Check if download was successful
        if (err == ESP_OK && status_code == 200 && total_downloaded > 0) {
            ESP_LOGI(TAG, "Downloaded %d bytes (content_length: %d), content_type: %s",
                     total_downloaded, content_length, content_type);
            break;  // Success, exit retry loop
        }

        // Log the error for this attempt
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        } else if (status_code != 200) {
            ESP_LOGE(TAG, "HTTP request failed with status code: %d", status_code);
        } else if (total_downloaded <= 0) {
            ESP_LOGE(TAG, "No data downloaded from URL");
        }

        // Clean up failed download (don't free content_type - it's reused across retries)
        unlink(temp_upload_path);
    }
    // Check final result after all retries
    if (err != ESP_OK || status_code != 200 || total_downloaded <= 0) {
        ESP_LOGE(TAG, "Failed to download image after %d attempts", max_retries);
        // Store descriptive error for UI display
        char err_msg[256];
        if (err != ESP_OK) {
            const char *err_name = esp_err_to_name(err);
            if (err == ESP_ERR_HTTP_CONNECT) {
                snprintf(err_msg, sizeof(err_msg), "Connection failed (%s)", err_name);
            } else {
                snprintf(err_msg, sizeof(err_msg), "%s", err_name);
            }
        } else if (status_code != 200) {
            snprintf(err_msg, sizeof(err_msg), "Server returned HTTP %d", status_code);
        } else {
            snprintf(err_msg, sizeof(err_msg), "No data received from server");
        }
        utils_set_last_fetch_error(err_msg);
        free(content_type);
        free(thumbnail_url_buffer);
        free(config_payload_buffer);
        free(etag_buffer);
        unlink(temp_upload_path);
        return ESP_FAIL;
    }

    // Persist the ETag from this successful 200 response (or clear if the server
    // dropped it) so the next request can send If-None-Match.
    config_manager_set_image_etag(etag_buffer);
    free(etag_buffer);

    // Detect format regardless of Content-Type (which might be unreliable),
    // falling back to the header only when the magic-byte check fails
    *format = image_processor_detect_format(temp_upload_path);
    if (*format == IMAGE_FORMAT_UNKNOWN) {
        if (strcmp(content_type, "image/bmp") == 0)
            *format = IMAGE_FORMAT_BMP;
        else if (strcmp(content_type, "image/png") == 0)
            *format = IMAGE_FORMAT_PNG;
        else if (strcmp(content_type, "image/jpeg") == 0)
            *format = IMAGE_FORMAT_JPG;
    }
    free(content_type);

    *thumbnail_url_out = thumbnail_url_buffer;
    *config_payload_out = config_payload_buffer;
    return ESP_OK;
}

// Fetch the server-provided thumbnail into the .current.jpg slot; returns
// whether it now holds a thumbnail for the image being displayed
static bool fetch_download_thumbnail(const char *thumbnail_url)
{
    ESP_LOGI(TAG, "Downloading thumbnail from: %s", thumbnail_url);

    const char *temp_jpg_path = CURRENT_JPG_PATH;
    FILE *thumb_file = fopen(temp_jpg_path, "wb");
    if (!thumb_file) {
        return false;
    }

    char thumb_content_type[128] = {0};
    download_context_t thumb_ctx = {.file = thumb_file,
                                    .total_read = 0,
                                    .content_type = thumb_content_type,
                                    .thumbnail_url = NULL,
                                    .config_payload = NULL,
                                    .etag = NULL};

    esp_http_client_config_t thumb_config = {
        .url = thumbnail_url,
        .timeout_ms = 30000,
        .event_handler = http_event_handler,
        .user_data = &thumb_ctx,
        .max_redirection_count = 5,
        .user_agent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36",
    };

    esp_http_client_handle_t thumb_client = esp_http_client_init(&thumb_config);
    if (!thumb_client) {
        fclose(thumb_file);
        unlink(temp_jpg_path);
        return false;
    }

    // Authenticate the thumbnail fetch the same way as the image fetch --
    // the X-Thumbnail-URL is served by the same host, which may be
    // token-gated.
    const char *thumb_token = config_manager_get_access_token();
    if (thumb_token && strlen(thumb_token) > 0) {
        char thumb_auth[ACCESS_TOKEN_MAX_LEN + 20];
        snprintf(thumb_auth, sizeof(thumb_auth), "Bearer %s", thumb_token);
        esp_http_client_set_header(thumb_client, "Authorization", thumb_auth);
    }
    const char *thumb_hk = config_manager_get_http_header_key();
    const char *thumb_hv = config_manager_get_http_header_value();
    if (thumb_hk && strlen(thumb_hk) > 0 && thumb_hv && strlen(thumb_hv) > 0 &&
        !(strcasecmp(thumb_hk, "Authorization") == 0 && thumb_token && strlen(thumb_token) > 0)) {
        esp_http_client_set_header(thumb_client, thumb_hk, thumb_hv);
    }

    esp_err_t thumb_err = esp_http_client_perform(thumb_client);
    int thumb_status = esp_http_client_get_status_code(thumb_client);

    fclose(thumb_file);
    esp_http_client_cleanup(thumb_client);

    if (thumb_err == ESP_OK && thumb_status == 200 && thumb_ctx.total_read > 0) {
        ESP_LOGI(TAG, "Thumbnail downloaded successfully: %d bytes", thumb_ctx.total_read);
        return true;
    }

    ESP_LOGW(TAG, "Failed to download thumbnail (status: %d)", thumb_status);
    unlink(temp_jpg_path);
    return false;
}

// Apply a remote config payload received from the server. Expected
// structure: { "config": {...}, "processing_settings": {...},
// "color_palette": {...} }
static void fetch_apply_remote_config(const char *config_payload)
{
    cJSON *payload = cJSON_Parse(config_payload);
    if (!payload) {
        ESP_LOGE(TAG, "Failed to parse config payload JSON");
        return;
    }

    bool applied = false;

    cJSON *config_obj = cJSON_GetObjectItem(payload, "config");
    if (config_obj && cJSON_IsObject(config_obj)) {
        apply_config_from_json(config_obj);
        applied = true;
    }

    cJSON *proc_obj = cJSON_GetObjectItem(payload, "processing_settings");
    if (proc_obj && cJSON_IsObject(proc_obj)) {
        processing_settings_t settings;
        processing_settings_get_defaults(&settings);
        processing_settings_from_json(proc_obj, &settings);
        processing_settings_save(&settings);
        applied = true;
    }

    cJSON *palette_obj = cJSON_GetObjectItem(payload, "color_palette");
    if (palette_obj && cJSON_IsObject(palette_obj)) {
        color_palette_t palette;
        color_palette_get_defaults(&palette);
        color_palette_from_json(palette_obj, &palette);
        color_palette_save(&palette);
        image_processor_reload_palette();
        applied = true;
    }

    cJSON_Delete(payload);

    if (applied) {
        config_manager_touch_config();
        ESP_LOGI(TAG, "Remote config payload applied successfully");
    }
}

// Stream a downloaded PNG/JPG straight to the display -- no processed file
// and no process-to-file round-trip (the old flow zlib-encoded a panel-size
// PNG only for show_image to re-decode it). A pre-processed PNG displays in
// a single validating decode; anything else is processed. With album saving
// on, the finished 4bpp frame is snapshotted to the album as .epdgz right
// after the refresh, while the display mutex is still held.
static esp_err_t fetch_stream_display(image_format_t image_format, bool thumbnail_downloaded)
{
    const char *temp_upload_path = CURRENT_UPLOAD_PATH;
    const char *temp_jpg_path = CURRENT_JPG_PATH;
    const char *temp_png_path = CURRENT_PNG_PATH;

    dither_algorithm_t algo = processing_settings_get_dithering_algorithm();

    bool persistent = storage_has_persistent_storage();
    bool save_to_album = persistent && config_manager_get_save_downloaded_images();

    // Album paths are decided before display so the current-image link
    // records the final logical name atomically with the refresh
    char album_image_path[512] = {0};
    char album_thumb_path[512] = {0};
    if (save_to_album) {
        char downloads_path[256];
        snprintf(downloads_path, sizeof(downloads_path), "%s/Downloads", IMAGE_DIRECTORY);
        struct stat st;
        if (stat(downloads_path, &st) != 0 && mkdir(downloads_path, 0755) != 0) {
            ESP_LOGW(TAG, "Failed to create Downloads directory, not saving to album");
            save_to_album = false;
        } else {
            time_t now = time(NULL);
            snprintf(album_image_path, sizeof(album_image_path), "%s/download_%lld.epdgz",
                     downloads_path, (long long) now);
            snprintf(album_thumb_path, sizeof(album_thumb_path), "%s/download_%lld.jpg",
                     downloads_path, (long long) now);
        }
    }

    // An album .epdgz has no browser-renderable preview unless a thumbnail
    // exists (downloaded, or the JPG original); without one, the
    // current-image link points at the kept original instead
    bool album_has_preview = thumbnail_downloaded || image_format == IMAGE_FORMAT_JPG;

    // JPG sources are read into RAM up front so the original file is free
    // to be staged as the album preview before display
    uint8_t *file_buffer = NULL;
    size_t file_size = 0;
    if (image_format == IMAGE_FORMAT_JPG) {
        esp_err_t read_err = display_flow_read_file(temp_upload_path, &file_buffer, &file_size);
        if (read_err != ESP_OK) {
            unlink(temp_upload_path);
            return read_err;
        }
        if (!persistent) {
            // MemFS-backed source lives in PSRAM; drop the file now that
            // the compressed copy exists
            unlink(temp_upload_path);
        }
    }

    // Stage the album preview BEFORE display: end_rgb_stream publishes the
    // album link under the display mutex, and the link's .jpg sibling must
    // already exist at that moment or /api/current_image can 404
    // (transiently, or permanently if the move fails)
    bool preview_staged = false;
    if (save_to_album && album_has_preview) {
        if (thumbnail_downloaded) {
            preview_staged = rename(temp_jpg_path, album_thumb_path) == 0;
        } else {
            preview_staged = rename(temp_upload_path, album_thumb_path) == 0;
        }
        if (!preview_staged) {
            ESP_LOGW(TAG, "Failed to stage album thumbnail; keeping original as preview");
            album_has_preview = false;
        }
    }

    // The fallback name is what end_rgb_stream publishes -- atomically,
    // under the display mutex -- if the album snapshot fails, matching the
    // keep-original disposal below
    const char *fallback_name =
        (persistent && image_format == IMAGE_FORMAT_JPG) ? temp_jpg_path : temp_png_path;

    display_publish_t pub = {
        .display_name = (save_to_album && album_has_preview) ? album_image_path : fallback_name,
        .save_path = save_to_album ? album_image_path : NULL,
        .fallback_name = fallback_name,
    };

    esp_err_t err;
    if (image_format == IMAGE_FORMAT_PNG) {
        // File-backed fused path: no RAM copy of the download; MemFS
        // sources are released as soon as processing copies them
        err = display_flow_stream_file(temp_upload_path, image_format, algo, &pub, !persistent);
    } else {
        err = image_processor_process_to_display(file_buffer, file_size, image_format, algo, &pub);
        heap_caps_free(file_buffer);
    }

    if (err == ESP_ERR_NOT_FINISHED) {
        // Displayed, but the album snapshot failed (e.g. storage full):
        // end_rgb_stream already published the fallback name; fall back to
        // the keep-original disposal so that name resolves
        ESP_LOGW(TAG, "Album snapshot failed; keeping download as current image only");
        if (preview_staged) {
            // Bring the staged album preview back as the current thumbnail
            // so the fallback link resolves
            if (rename(album_thumb_path, temp_jpg_path) == 0) {
                thumbnail_downloaded = true;
            } else {
                ESP_LOGW(TAG, "Failed to restore staged album thumbnail");
                unlink(album_thumb_path);
            }
            preview_staged = false;
        }
        save_to_album = false;
        err = ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to process and display image: %s", esp_err_to_name(err));
        unlink(temp_upload_path);
        if (preview_staged) {
            unlink(album_thumb_path);
        }
        return err;
    }

    // The new image is on the panel; only now may the previous display's
    // files be replaced or dropped
    if (save_to_album) {
        if (!album_has_preview) {
            // No renderable album preview exists: keep the original in the
            // published current-image slot for its format
            display_flow_retire_source(temp_upload_path, image_format, false);
        } else {
            // The album holds both the image and its preview; nothing from
            // this download stays in the .current.* scheme
            unlink(temp_upload_path);
            display_flow_drop_stale_current(NULL, false);
        }
        ESP_LOGI(TAG, "Saved to Downloads album: %s", album_image_path);
    } else {
        // Keep-original policy, matching the direct display endpoint
        display_flow_retire_source(temp_upload_path, image_format, thumbnail_downloaded);
    }

    ESP_LOGI(TAG, "Image displayed via stream");
    utils_set_last_fetch_error(NULL);
    return ESP_OK;
}

// Display a downloaded EPDGZ/BMP from its file (these formats are already
// display-ready), optionally moving it into the Downloads album first
static esp_err_t fetch_display_file(image_format_t image_format, bool thumbnail_fresh)
{
    const char *staged = display_flow_stage_file(CURRENT_UPLOAD_PATH, image_format);
    if (!staged) {
        return ESP_FAIL;
    }

    char display_path[512];
    snprintf(display_path, sizeof(display_path), "%s", staged);

    // Optionally move into the Downloads album (with the downloaded
    // thumbnail alongside, so the album entry has a preview)
    if (storage_has_persistent_storage() && config_manager_get_save_downloaded_images()) {
        char downloads_path[256];
        snprintf(downloads_path, sizeof(downloads_path), "%s/Downloads", IMAGE_DIRECTORY);

        struct stat st;
        if (stat(downloads_path, &st) != 0 && mkdir(downloads_path, 0755) != 0) {
            ESP_LOGW(TAG, "Failed to create Downloads directory, using temp path");
        } else {
            time_t now = time(NULL);
            char filename_base[64];
            snprintf(filename_base, sizeof(filename_base), "download_%lld", (long long) now);

            const char *save_ext = (image_format == IMAGE_FORMAT_EPD_GZ) ? ".epdgz" : ".bmp";
            char final_image_path[512];
            snprintf(final_image_path, sizeof(final_image_path), "%s/%s%s", downloads_path,
                     filename_base, save_ext);

            if (rename(staged, final_image_path) != 0) {
                ESP_LOGW(TAG, "Failed to move image to Downloads album, using temp path");
            } else {
                snprintf(display_path, sizeof(display_path), "%s", final_image_path);

                // Move the thumbnail to the album if we moved the main image
                bool thumbnail_saved_to_album = false;
                struct stat thumb_st;
                if (stat(CURRENT_JPG_PATH, &thumb_st) == 0) {
                    char final_thumb_path[512];
                    snprintf(final_thumb_path, sizeof(final_thumb_path), "%s/%s.jpg",
                             downloads_path, filename_base);
                    if (rename(CURRENT_JPG_PATH, final_thumb_path) == 0) {
                        thumbnail_saved_to_album = true;
                    } else {
                        ESP_LOGW(TAG, "Failed to move thumbnail to Downloads album");
                    }
                }

                if (thumbnail_saved_to_album) {
                    ESP_LOGI(TAG, "Saved to Downloads album: %s (with thumbnail)", filename_base);
                } else {
                    ESP_LOGI(TAG, "Saved to Downloads album: %s", filename_base);
                }
            }
        }
    }

    ESP_LOGI(TAG, "Successfully processed image, displaying: %s", display_path);
    if (display_manager_show_image(display_path) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to display fetched image");
        // Drop a file still in its staged .current.* slot: a previous
        // display's link may point at this name, and it must not resolve to
        // the failed download. An album-moved file is left in the album.
        if (strcmp(display_path, staged) == 0) {
            unlink(display_path);
        }
        utils_set_last_fetch_error("Failed to display fetched image");
        return ESP_FAIL;
    }

    // Keep the displayed .current file so /api/current_image can serve the
    // original (matching the direct-display policy); drop the stale
    // siblings. Album saves already moved theirs. Cleanup runs only after a
    // successful display, so a failure keeps the previous image's files
    // (and thumbnail) intact.
    display_flow_drop_stale_current(display_path, thumbnail_fresh);

    utils_set_last_fetch_error(NULL);  // Clear error on success
    return ESP_OK;
}

// Generates a blank white canvas at the panel's native resolution and
// overlays the message on it - used whenever there's no existing displayed
// image to overlay onto (fresh boot, after /clear, or a non-overlay-ready
// current image). White is a valid palette entry on every supported panel,
// so the buffer is already "processed" as far as image_processor_draw_caption
// is concerned.
static esp_err_t display_error_overlay_blank(const char *message)
{
    int width = BOARD_HAL_DISPLAY_WIDTH;
    int height = BOARD_HAL_DISPLAY_HEIGHT;
    size_t buf_size = (size_t) width * (size_t) height * 3;

    uint8_t *rgb_buffer = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!rgb_buffer) {
        ESP_LOGE(TAG, "Failed to allocate blank canvas for error overlay");
        return ESP_ERR_NO_MEM;
    }
    memset(rgb_buffer, 0xFF, buf_size);

    image_processor_draw_caption(rgb_buffer, width, height, message);
    esp_err_t err = image_processor_write_rgb_to_png(rgb_buffer, width, height, CURRENT_PNG_PATH);
    heap_caps_free(rgb_buffer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write blank error-overlay canvas: %s", esp_err_to_name(err));
        return err;
    }

    display_manager_show_image(CURRENT_PNG_PATH);
    ESP_LOGW(TAG, "Displayed error overlay on a blank canvas: %s", message);
    return ESP_OK;
}

// Overlays a short message on the currently displayed image WITHOUT modifying
// the original saved file: copies it to the scratch PNG path first, draws the
// caption there, and displays the copy. Falls back to a blank canvas (see
// above) if there's nothing suitable to overlay onto.
static esp_err_t display_error_overlay(const char *message)
{
    const char *current_image = display_manager_get_current_image();
    if (!current_image || current_image[0] == '\0') {
        ESP_LOGI(TAG, "No current image to overlay error onto, using a blank canvas");
        return display_error_overlay_blank(message);
    }

    image_format_t format = image_processor_detect_format(current_image);
    if (format != IMAGE_FORMAT_PNG || !image_processor_is_processed(current_image)) {
        ESP_LOGI(TAG, "Current image %s is not an overlay-ready processed PNG, using a blank canvas",
                 current_image);
        return display_error_overlay_blank(message);
    }

    FILE *src = fopen(current_image, "rb");
    if (!src) {
        ESP_LOGE(TAG, "Failed to open %s for error overlay", current_image);
        return ESP_FAIL;
    }
    FILE *dst = fopen(CURRENT_PNG_PATH, "wb");
    if (!dst) {
        fclose(src);
        ESP_LOGE(TAG, "Failed to open %s for error overlay", CURRENT_PNG_PATH);
        return ESP_FAIL;
    }
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
        fwrite(buf, 1, n, dst);
    }
    fclose(src);
    fclose(dst);

    image_processor_add_caption_to_file(CURRENT_PNG_PATH, message);
    display_manager_show_image(CURRENT_PNG_PATH);
    ESP_LOGW(TAG, "Displayed error overlay: %s", message);
    return ESP_OK;
}

void utils_handle_wifi_connect_result(bool connected)
{
    if (connected) {
        if (config_manager_get_wifi_fail_count() != 0) {
            config_manager_set_wifi_fail_count(0);
        }
        return;
    }

    int count = config_manager_get_wifi_fail_count() + 1;
    config_manager_set_wifi_fail_count(count);
    ESP_LOGW(TAG, "WiFi connect failed (%d consecutive)", count);

    if (!config_manager_get_error_overlay_enabled() || count < WIFI_FAIL_OVERLAY_THRESHOLD) {
        return;
    }

    char caption[96];
    snprintf(caption, sizeof(caption), "Error: No WiFi connection (%dx in a row)", count);
    display_error_overlay(caption);
}

esp_err_t utils_test_error_overlay(void)
{
    // Manual preview from the Web UI - always overlays the example message
    // regardless of the error-overlay setting or the WiFi-fail counter, so
    // it can be used to see what the feature looks like before enabling it.
    return display_error_overlay("Error: No WiFi connection (3x in a row) - TEST");
}

esp_err_t fetch_and_display_image_from_url(const char *url, bool *not_modified)
{
    ESP_LOGI(TAG, "Fetching image from URL: %s", url);

    if (not_modified) {
        *not_modified = false;
    }

    image_format_t image_format = IMAGE_FORMAT_UNKNOWN;
    char *thumbnail_url = NULL;
    char *config_payload = NULL;
    bool was_not_modified = false;
    esp_err_t err = fetch_perform_download(url, &was_not_modified, &image_format, &thumbnail_url,
                                           &config_payload);
    if (err != ESP_OK) {
        return err;
    }
    if (was_not_modified) {
        if (not_modified) {
            *not_modified = true;
        }
        return ESP_OK;
    }

    bool thumbnail_downloaded = false;
    if (thumbnail_url && strlen(thumbnail_url) > 0) {
        thumbnail_downloaded = fetch_download_thumbnail(thumbnail_url);
    }
    free(thumbnail_url);

    if (config_payload && strlen(config_payload) > 0) {
        fetch_apply_remote_config(config_payload);
    }
    free(config_payload);

    switch (image_format) {
    case IMAGE_FORMAT_PNG:
    case IMAGE_FORMAT_JPG:
        return fetch_stream_display(image_format, thumbnail_downloaded);
    case IMAGE_FORMAT_EPD_GZ:
    case IMAGE_FORMAT_BMP:
        return fetch_display_file(image_format, thumbnail_downloaded);
    default:
        ESP_LOGE(TAG, "Unsupported image format: %d", image_format);
        unlink(CURRENT_UPLOAD_PATH);
        return ESP_FAIL;
    }
}

esp_err_t trigger_image_rotation(void)
{
    rotation_mode_t rotation_mode = config_manager_get_rotation_mode();
    esp_err_t result = ESP_OK;

    if (rotation_mode == ROTATION_MODE_TELEGRAM) {
        // Telegram mode - poll getUpdates, download+display the newest image
        // (with progressive-size fallback), queue any "/" commands.
        telegram_poll_result_t poll_result = TELEGRAM_POLL_ERROR;
        esp_err_t poll_err = telegram_bot_poll(&poll_result);

        if (poll_result == TELEGRAM_POLL_RESET) {
            // Emergency "/telegram_reset": the queue was already cleared and
            // acknowledged inside telegram_bot_poll() - skip HA notify, the
            // post-rotate HTTP window, everything, and sleep right now.
            ESP_LOGW(TAG, "Telegram emergency reset - entering deep sleep immediately");
            power_manager_enter_sleep();
            // Not reached.
        }

        if (poll_err == ESP_OK) {
            utils_set_last_fetch_error(NULL);
            if (poll_result == TELEGRAM_POLL_OK_NO_IMAGE) {
                // No new Telegram image this cycle - still change the
                // display, same as the non-Telegram rotation modes, by
                // falling back to the active album(s) (this also covers the
                // Telegram download folder, which shows up as a regular
                // album - see telegram_bot_poll()).
                ESP_LOGI(TAG, "No new Telegram image, falling back to local rotation");

                char prev_image[64];
                const char *before = display_manager_get_current_image();
                strncpy(prev_image, before ? before : "", sizeof(prev_image) - 1);
                prev_image[sizeof(prev_image) - 1] = '\0';

                display_manager_rotate_from_storage();

                // Only notify if the display actually changed - rotation is
                // a no-op when there are no enabled albums / no images.
                const char *after = display_manager_get_current_image();
                if (config_manager_get_telegram_rotation_notify_enabled() && after &&
                    after[0] != '\0' && strcmp(after, prev_image) != 0) {
                    telegram_bot_notify_fallback_image(after);
                }
            }
            result = ESP_OK;
        } else {
            const char *reason = (poll_result == TELEGRAM_POLL_NOT_CONFIGURED)
                                     ? "Telegram bot not configured"
                                     : "Telegram poll failed";
            ESP_LOGW(TAG, "%s, falling back to local rotation", reason);
            utils_set_last_fetch_error(reason);
            display_manager_rotate_from_storage();
            result = ESP_FAIL;
        }
    } else if (rotation_mode == ROTATION_MODE_URL) {
        // URL mode - fetch image from URL
        const char *image_url = config_manager_get_image_url();
        ESP_LOGI(TAG, "URL rotation mode - downloading from: %s", image_url);

        bool not_modified = false;
        if (fetch_and_display_image_from_url(image_url, &not_modified) == ESP_OK) {
            if (not_modified) {
                // Server confirmed cached image still current (HTTP 304).
                // Keep the existing eInk image — do not refresh, do not fall
                // back to SD rotation.
                ESP_LOGI(TAG, "Image unchanged on server, skipping display refresh");
            }
        } else {
            ESP_LOGE(TAG,
                     "Failed to fetch and display image from URL, falling back to local rotation");
            display_manager_rotate_from_storage();
            result = ESP_FAIL;
        }
    } else {
        // Local storage mode - rotate through albums
        display_manager_rotate_from_storage();
        result = ESP_OK;
    }

    return result;
}

cJSON *create_battery_json(void)
{
    cJSON *json = cJSON_CreateObject();
    if (json == NULL) {
        return NULL;
    }

    int battery_percent = board_hal_get_battery_percent();
    int battery_voltage = board_hal_get_battery_voltage();
    bool is_charging = board_hal_is_charging();
    bool usb_connected = board_hal_is_usb_connected();
    bool battery_connected = board_hal_is_battery_connected();

    cJSON_AddNumberToObject(json, "battery_level", battery_percent);
    cJSON_AddNumberToObject(json, "battery_voltage", battery_voltage);
    cJSON_AddBoolToObject(json, "charging", is_charging);
    cJSON_AddBoolToObject(json, "usb_connected", usb_connected);
    cJSON_AddBoolToObject(json, "battery_connected", battery_connected);

    return json;
}

int get_seconds_until_next_wakeup(void)
{
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    cron_rule_t rules[MAX_CRON_RULES];
    int n = config_manager_get_compiled_cron_rules(rules, MAX_CRON_RULES);
    if (n == 0) {
        return CRON_FALLBACK_SEC;
    }

    return cron_seconds_until_next(&timeinfo, rules, n);
}

void sanitize_hostname(const char *device_name, char *hostname, size_t max_len)
{
    size_t i = 0, j = 0;
    bool last_was_hyphen = false;

    while (device_name[i] != '\0' && j < max_len - 1) {
        char c = device_name[i];

        if ((c >= 'A' && c <= 'Z')) {
            // Uppercase: convert to lowercase
            hostname[j++] = c + 32;
            last_was_hyphen = false;
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            // Lowercase letters and digits: keep as-is
            hostname[j++] = c;
            last_was_hyphen = false;
        } else if (!last_was_hyphen && j > 0) {
            // Replace spaces and special characters with hyphen
            // But avoid leading hyphens or consecutive hyphens
            hostname[j++] = '-';
            last_was_hyphen = true;
        }

        i++;
    }

    // Remove trailing hyphen if present
    if (j > 0 && hostname[j - 1] == '-') {
        j--;
    }

    hostname[j] = '\0';

    // If result is empty, use default
    if (j == 0) {
        strncpy(hostname, "photoframe", max_len - 1);
        hostname[max_len - 1] = '\0';
    }
}

void sanitize_dhcp_hostname(const char *device_name, char *hostname, size_t max_len)
{
    size_t i = 0, j = 0;

    while (device_name[i] != '\0' && j < max_len - 1) {
        char c = device_name[i];

        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            hostname[j++] = c;
        }

        i++;
    }

    hostname[j] = '\0';

    // If result is empty, use default
    if (j == 0) {
        strncpy(hostname, "PhotoFrame", max_len - 1);
        hostname[max_len - 1] = '\0';
    }
}

const char *get_device_id(void)
{
    static char device_id[13];
    static bool id_fetched = false;

    if (!id_fetched) {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(device_id, sizeof(device_id), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2],
                 mac[3], mac[4], mac[5]);
        id_fetched = true;
    }

    return device_id;
}

const char *get_setup_ap_ssid(void)
{
    static char ap_ssid[32];
    static bool built = false;

    if (!built) {
        const char *id = get_device_id();
        // Use last 5 hex chars of device ID, uppercased
        char short_id[6];
        strncpy(short_id, id + 7, 5);
        short_id[5] = '\0';
        for (int i = 0; i < 5; i++) {
            if (short_id[i] >= 'a' && short_id[i] <= 'f')
                short_id[i] -= 32;
        }
        snprintf(ap_ssid, sizeof(ap_ssid), "PhotoFrame - %s", short_id);
        built = true;
    }

    return ap_ssid;
}
