#include "utils.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "board_hal.h"
#include "cJSON.h"
#include "color_palette.h"
#include "config.h"
#include "config_manager.h"
#include "display_manager.h"
#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "image_processor.h"
#include "mdns_service.h"
#include "periodic_tasks.h"
#include "power_manager.h"
#include "processing_settings.h"
#include "storage.h"
#include "testable_utils.h"
#include "wifi_manager.h"

static const char *TAG = "utils";

// Last image fetch error (transient, not persisted)
static char last_fetch_error[256] = {0};

void utils_set_last_fetch_error(const char *error)
{
    if (error) {
        strncpy(last_fetch_error, error, sizeof(last_fetch_error) - 1);
        last_fetch_error[sizeof(last_fetch_error) - 1] = '\0';
    } else {
        last_fetch_error[0] = '\0';
    }
}

const char *utils_get_last_fetch_error(void)
{
    return last_fetch_error;
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
        }
    }

    item = cJSON_GetObjectItem(root, "timezone");
    if (item && cJSON_IsString(item)) {
        const char *tz = cJSON_GetStringValue(item);
        config_manager_set_timezone(tz);
        setenv("TZ", tz, 1);
        tzset();
    }

    item = cJSON_GetObjectItem(root, "ntp_server");
    if (item && cJSON_IsString(item)) {
        config_manager_set_ntp_server(cJSON_GetStringValue(item));
        periodic_tasks_force_run(SNTP_TASK_NAME);
        periodic_tasks_check_and_run();
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
        config_manager_set_display_rotation_deg(item->valueint);
        display_manager_initialize_paint();
    }

    // Auto Rotate
    item = cJSON_GetObjectItem(root, "auto_rotate");
    if (item && cJSON_IsBool(item)) {
        config_manager_set_auto_rotate(cJSON_IsTrue(item));
        power_manager_reset_rotate_timer();
    }

    item = cJSON_GetObjectItem(root, "rotate_interval");
    if (item && cJSON_IsNumber(item)) {
        config_manager_set_rotate_interval(item->valueint);
        power_manager_reset_rotate_timer();
    }

    item = cJSON_GetObjectItem(root, "auto_rotate_aligned");
    if (item && cJSON_IsBool(item)) {
        config_manager_set_auto_rotate_aligned(cJSON_IsTrue(item));
        power_manager_reset_rotate_timer();
    }

    // Advanced Auto Rotate
    item = cJSON_GetObjectItem(root, "ar_mode");
    if (item && cJSON_IsString(item)) {
        const char *mode_str = cJSON_GetStringValue(item);
        if (strcmp(mode_str, "daily") == 0) {
            config_manager_set_ar_mode(AR_MODE_DAILY);
        } else {
            config_manager_set_ar_mode(AR_MODE_INTERVAL);
        }
        power_manager_reset_rotate_timer();
    }

    item = cJSON_GetObjectItem(root, "ar_start_time");
    if (item && cJSON_IsNumber(item)) {
        config_manager_set_ar_start_time(item->valueint);
        power_manager_reset_rotate_timer();
    }

    item = cJSON_GetObjectItem(root, "ar_policy");
    if (item && cJSON_IsString(item)) {
        const char *policy_str = cJSON_GetStringValue(item);
        if (strcmp(policy_str, "sequential") == 0) {
            config_manager_set_ar_sleep_policy(AR_POLICY_SEQUENTIAL);
        } else {
            config_manager_set_ar_sleep_policy(AR_POLICY_SYNCHRONIZED);
        }
        power_manager_reset_rotate_timer();
    }

    item = cJSON_GetObjectItem(root, "ar_anchor");
    if (item && cJSON_IsBool(item)) {
        config_manager_set_ar_use_anchor(cJSON_IsTrue(item));
        power_manager_reset_rotate_timer();
    }

    item = cJSON_GetObjectItem(root, "auto_rotate_aligned");
    if (item && cJSON_IsBool(item)) {
        config_manager_set_auto_rotate_aligned(cJSON_IsTrue(item));
        power_manager_reset_rotate_timer();
    }

    item = cJSON_GetObjectItem(root, "sleep_schedule_enabled");
    if (item && cJSON_IsBool(item)) {
        config_manager_set_sleep_schedule_enabled(cJSON_IsTrue(item));
    }

    item = cJSON_GetObjectItem(root, "sleep_schedule_start");
    if (item && cJSON_IsNumber(item)) {
        config_manager_set_sleep_schedule_start(item->valueint);
    }

    item = cJSON_GetObjectItem(root, "sleep_schedule_end");
    if (item && cJSON_IsNumber(item)) {
        config_manager_set_sleep_schedule_end(item->valueint);
    }

    item = cJSON_GetObjectItem(root, "rotation_mode");
    if (item && cJSON_IsString(item)) {
        const char *mode_str = cJSON_GetStringValue(item);
        rotation_mode_t mode = ROTATION_MODE_STORAGE;
        if (strcmp(mode_str, "url") == 0)
            mode = ROTATION_MODE_URL;
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

    // Auto Rotate - URL
    item = cJSON_GetObjectItem(root, "image_url");
    if (item && cJSON_IsString(item)) {
        config_manager_set_image_url(cJSON_GetStringValue(item));
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

    return ESP_OK;
}

// Context for HTTP event handler
typedef struct {
    FILE *file;
    int total_read;
    char *content_type;
    char *thumbnail_url;   // Optional thumbnail URL from X-Thumbnail-URL header
    char *config_payload;  // Optional config JSON from X-Config-Payload header
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
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

esp_err_t fetch_and_save_image_from_url(const char *url, char *saved_image_path, size_t path_size)
{
    ESP_LOGI(TAG, "Fetching image from URL: %s", url);

    // Use fixed paths for current image and upload
    const char *temp_jpg_path = CURRENT_JPG_PATH;
    const char *temp_upload_path = CURRENT_UPLOAD_PATH;
    const char *temp_bmp_path = CURRENT_BMP_PATH;
    const char *temp_png_path = CURRENT_PNG_PATH;

    esp_err_t err = ESP_FAIL;
    int status_code = 0;
    int content_length = 0;
    char *content_type = NULL;
    char *thumbnail_url_buffer = NULL;
    int total_downloaded = 0;
    const int max_retries = 3;

    char *config_payload_buffer = NULL;

    // Allocate buffers once before retry loop
    thumbnail_url_buffer = calloc(512, 1);
    content_type = calloc(128, 1);
    config_payload_buffer = calloc(2048, 1);

    if (!content_type || !thumbnail_url_buffer || !config_payload_buffer) {
        ESP_LOGE(TAG, "Failed to allocate memory for download context");
        free(content_type);
        free(thumbnail_url_buffer);
        free(config_payload_buffer);
        return ESP_FAIL;
    }

    // Retry loop
    for (int retry = 0; retry < max_retries; retry++) {
        if (retry > 0) {
            ESP_LOGW(TAG, "Retry attempt %d/%d after 2 second delay...", retry + 1, max_retries);
            vTaskDelay(pdMS_TO_TICKS(2000));  // 2 second delay between retries
        }

        FILE *file = fopen(temp_upload_path, "wb");
        if (!file) {
            ESP_LOGE(TAG, "Failed to open file for writing: %s", temp_upload_path);
            continue;  // Try again
        }

        // Clear buffers for this retry
        memset(content_type, 0, 128);
        memset(config_payload_buffer, 0, 2048);

        download_context_t ctx = {.file = file,
                                  .total_read = 0,
                                  .content_type = content_type,
                                  .thumbnail_url = thumbnail_url_buffer,
                                  .config_payload = config_payload_buffer};

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

        // Add hostname header (mDNS name with .local suffix)
        char hostname[64];
        sanitize_hostname(config_manager_get_device_name(), hostname, sizeof(hostname) - 6);
        strlcat(hostname, ".local", sizeof(hostname));
        esp_http_client_set_header(client, "X-Hostname", hostname);

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

        err = esp_http_client_perform(client);

        status_code = esp_http_client_get_status_code(client);
        content_length = esp_http_client_get_content_length(client);
        total_downloaded = ctx.total_read;
        content_type = ctx.content_type;

        fclose(file);
        esp_http_client_cleanup(client);

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
        unlink(temp_upload_path);
        return ESP_FAIL;
    }

    // Detect format regardless of Content-Type (which might be unreliable)
    image_format_t image_format = image_processor_detect_format(temp_upload_path);
    if (image_format == IMAGE_FORMAT_UNKNOWN) {
        // Fallback to Content-Type if detection failed or file is empty?
        // Actually, detect_format is more reliable. If it fails, we trust it.
        // But maybe we should check Content-Type as a hint if detection returned UNKNOWN?
        // For now, let's respect the user request to use detect_format.
        if (strcmp(content_type, "image/bmp") == 0)
            image_format = IMAGE_FORMAT_BMP;
        else if (strcmp(content_type, "image/png") == 0)
            image_format = IMAGE_FORMAT_PNG;
        else if (strcmp(content_type, "image/jpeg") == 0)
            image_format = IMAGE_FORMAT_JPG;
    }

    // Free content_type after successful processing
    free(content_type);

    // Track if thumbnail was successfully downloaded
    bool thumbnail_downloaded = false;

    // Download thumbnail if URL was provided in X-Thumbnail-URL header
    if (thumbnail_url_buffer && strlen(thumbnail_url_buffer) > 0) {
        ESP_LOGI(TAG, "Downloading thumbnail from: %s", thumbnail_url_buffer);

        FILE *thumb_file = fopen(temp_jpg_path, "wb");
        if (thumb_file) {
            char thumb_content_type[128] = {0};
            download_context_t thumb_ctx = {.file = thumb_file,
                                            .total_read = 0,
                                            .content_type = thumb_content_type,
                                            .thumbnail_url = NULL};

            esp_http_client_config_t thumb_config = {
                .url = thumbnail_url_buffer,
                .timeout_ms = 30000,
                .event_handler = http_event_handler,
                .user_data = &thumb_ctx,
                .max_redirection_count = 5,
                .user_agent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36",
            };

            esp_http_client_handle_t thumb_client = esp_http_client_init(&thumb_config);
            if (thumb_client) {
                esp_err_t thumb_err = esp_http_client_perform(thumb_client);
                int thumb_status = esp_http_client_get_status_code(thumb_client);

                fclose(thumb_file);
                esp_http_client_cleanup(thumb_client);

                if (thumb_err == ESP_OK && thumb_status == 200 && thumb_ctx.total_read > 0) {
                    ESP_LOGI(TAG, "Thumbnail downloaded successfully: %d bytes",
                             thumb_ctx.total_read);
                    thumbnail_downloaded = true;
                } else {
                    ESP_LOGW(TAG, "Failed to download thumbnail (status: %d)", thumb_status);
                    unlink(temp_jpg_path);
                }
            } else {
                fclose(thumb_file);
                unlink(temp_jpg_path);
            }
        }
    }

    // Free thumbnail URL buffer
    if (thumbnail_url_buffer) {
        free(thumbnail_url_buffer);
    }

    // Apply remote config payload if received from server
    // Expected structure: { "config": {...}, "processing_settings": {...}, "color_palette": {...} }
    if (config_payload_buffer && strlen(config_payload_buffer) > 0) {
        cJSON *payload = cJSON_Parse(config_payload_buffer);
        if (payload) {
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
        } else {
            ESP_LOGE(TAG, "Failed to parse config payload JSON");
        }
    }
    free(config_payload_buffer);

    const char *final_path = NULL;

    // ========== STEP 1: Image Processing (always done first) ==========
    if (image_format == IMAGE_FORMAT_EPD_GZ) {
        // EPDGZ: already display-ready, just move to temp path (no processing needed)
        unlink(CURRENT_EPD_PATH);
        if (rename(temp_upload_path, CURRENT_EPD_PATH) != 0) {
            ESP_LOGE(TAG, "Failed to move EPDGZ to temp path");
            unlink(temp_upload_path);
            return ESP_FAIL;
        }
        final_path = CURRENT_EPD_PATH;
    } else if (image_format == IMAGE_FORMAT_BMP) {
        // BMP: just move to temp_bmp_path (no processing needed)
        unlink(temp_bmp_path);
        if (rename(temp_upload_path, temp_bmp_path) != 0) {
            ESP_LOGE(TAG, "Failed to move BMP to temp path");
            unlink(temp_upload_path);
            return ESP_FAIL;
        }
        final_path = temp_bmp_path;
    } else if (image_format == IMAGE_FORMAT_PNG || image_format == IMAGE_FORMAT_JPG) {
        bool needs_processing = true;
        if (image_format == IMAGE_FORMAT_PNG && image_processor_is_processed(temp_upload_path)) {
            needs_processing = false;
            ESP_LOGI(TAG, "Image already processed, skipping processing");
        }

        if (!needs_processing) {
            // Already processed PNG: just move to temp_png_path
            unlink(temp_png_path);
            if (rename(temp_upload_path, temp_png_path) != 0) {
                ESP_LOGE(TAG, "Failed to rename processed image");
                unlink(temp_upload_path);
                return ESP_FAIL;
            }
        } else {
            // Process the image to temp_png_path
            processing_settings_t settings;
            if (processing_settings_load(&settings) != ESP_OK) {
                processing_settings_get_defaults(&settings);
            }
            dither_algorithm_t algo = processing_settings_get_dithering_algorithm();

            if (storage_has_persistent_storage()) {
                // Persistent storage system: process to file
                err = image_processor_process(temp_upload_path, temp_png_path, algo);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to process image: %s", esp_err_to_name(err));
                    unlink(temp_upload_path);
                    return err;
                }
            } else {
                // Temporary/No-storage system: read file to buffer, process to RGB, display
                // directly
                FILE *fp = fopen(temp_upload_path, "rb");
                if (!fp) {
                    ESP_LOGE(TAG, "Failed to open uploaded file");
                    unlink(temp_upload_path);
                    return ESP_FAIL;
                }

                fseek(fp, 0, SEEK_END);
                long file_size = ftell(fp);
                fseek(fp, 0, SEEK_SET);

                uint8_t *file_buffer = heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM);
                if (!file_buffer) {
                    ESP_LOGE(TAG, "Failed to allocate buffer for image");
                    fclose(fp);
                    unlink(temp_upload_path);
                    return ESP_ERR_NO_MEM;
                }

                size_t bytes_read = fread(file_buffer, 1, file_size, fp);
                fclose(fp);

                if (bytes_read != (size_t) file_size) {
                    ESP_LOGE(TAG, "Incomplete file read: %zu of %ld bytes", bytes_read, file_size);
                    heap_caps_free(file_buffer);
                    unlink(temp_upload_path);
                    return ESP_ERR_INVALID_SIZE;
                }

                // For JPEG: save as thumbnail
                if (image_format == IMAGE_FORMAT_JPG && !thumbnail_downloaded) {
                    unlink(temp_jpg_path);
                    if (rename(temp_upload_path, temp_jpg_path) == 0) {
                        ESP_LOGI(TAG, "Using original JPEG as thumbnail: %s", temp_jpg_path);
                    } else {
                        unlink(temp_upload_path);
                    }
                } else {
                    unlink(temp_upload_path);
                }

                // Process to RGB buffer
                image_process_rgb_result_t result;
                err = image_processor_process_to_rgb(file_buffer, file_size, image_format, algo,
                                                     &result);
                heap_caps_free(file_buffer);

                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to process image: %s", esp_err_to_name(err));
                    return err;
                }

                // Display directly from RGB buffer
                err = display_manager_show_rgb_buffer(result.rgb_data, result.width, result.height);
                heap_caps_free(result.rgb_data);

                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to display image");
                    return err;
                }

                ESP_LOGI(TAG, "Image displayed from buffer");
                return ESP_OK;
            }
        }
        final_path = temp_png_path;

        // Handle thumbnail for JPEG: use original as thumbnail if none was downloaded
        if (image_format == IMAGE_FORMAT_JPG && !thumbnail_downloaded) {
            unlink(temp_jpg_path);
            if (rename(temp_upload_path, temp_jpg_path) != 0) {
                ESP_LOGW(TAG, "Failed to move original JPEG to thumbnail path");
                unlink(temp_upload_path);
            } else {
                ESP_LOGI(TAG, "Using original JPEG as thumbnail: %s", temp_jpg_path);
            }
        } else {
            // Clean up original upload file
            unlink(temp_upload_path);
        }
    } else {
        ESP_LOGE(TAG, "Unsupported image format: %d", image_format);
        unlink(temp_upload_path);
        return ESP_FAIL;
    }

    // ========== STEP 2: Optionally save to Downloads album ==========
    if (storage_has_persistent_storage()) {
        bool save_images = config_manager_get_save_downloaded_images();

        if (save_images) {
            char downloads_path[256];
            snprintf(downloads_path, sizeof(downloads_path), "%s/Downloads", IMAGE_DIRECTORY);

            // Create Downloads directory if it doesn't exist
            struct stat st;
            if (stat(downloads_path, &st) != 0) {
                ESP_LOGI(TAG, "Creating Downloads album directory");
                if (mkdir(downloads_path, 0755) != 0) {
                    ESP_LOGE(TAG, "Failed to create Downloads directory");
                    // Processing succeeded, just can't save to album - fall through to use temp
                    // path
                    goto use_temp_path;
                }
            }

            // Generate unique filename based on timestamp
            time_t now = time(NULL);
            char filename_base[64];
            snprintf(filename_base, sizeof(filename_base), "download_%lld", (long long) now);

            char final_image_path[512];
            bool thumbnail_saved_to_album = false;

            const char *save_ext = ".png";
            if (image_format == IMAGE_FORMAT_BMP) {
                save_ext = ".bmp";
            } else if (image_format == IMAGE_FORMAT_EPD_GZ) {
                save_ext = ".epdgz";
            }
            snprintf(final_image_path, sizeof(final_image_path), "%s/%s%s", downloads_path,
                     filename_base, save_ext);
            if (rename(final_path, final_image_path) != 0) {
                ESP_LOGW(TAG, "Failed to move image to Downloads album, using temp path");
            } else {
                final_path = NULL;  // Will be set below
            }

            // Move thumbnail to album if we successfully moved the main image
            if (final_path == NULL) {
                struct stat thumb_st;
                if (stat(temp_jpg_path, &thumb_st) == 0) {
                    char final_thumb_path[512];
                    snprintf(final_thumb_path, sizeof(final_thumb_path), "%s/%s.jpg",
                             downloads_path, filename_base);
                    if (rename(temp_jpg_path, final_thumb_path) == 0) {
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
                snprintf(saved_image_path, path_size, "%s", final_image_path);
            }
        }

    use_temp_path:
        // If not saved to album (or save failed), use temp path
        if (final_path != NULL) {
            snprintf(saved_image_path, path_size, "%s", final_path);
            ESP_LOGI(TAG, "Image processed (not saved to album): %s", final_path);
            if (thumbnail_downloaded) {
                ESP_LOGI(TAG, "Downloaded thumbnail available: %s", temp_jpg_path);
            }
        }
    } else {
        // No persistent storage support - just use temp path
        snprintf(saved_image_path, path_size, "%s", final_path);
        ESP_LOGI(TAG, "Image processed: %s", final_path);
        if (thumbnail_downloaded) {
            ESP_LOGI(TAG, "Downloaded thumbnail available: %s", temp_jpg_path);
        }
    }

    ESP_LOGI(TAG, "Successfully processed image: %s", saved_image_path);
    utils_set_last_fetch_error(NULL);  // Clear error on success

    return ESP_OK;
}

esp_err_t trigger_image_rotation(int skip_count)
{
    rotation_mode_t rotation_mode = config_manager_get_rotation_mode();
    esp_err_t result = ESP_OK;

    if (rotation_mode == ROTATION_MODE_URL) {
        // URL mode - fetch image from URL
        const char *image_url = config_manager_get_image_url();
        ESP_LOGI(TAG, "URL rotation mode - downloading from: %s", image_url);

        char saved_bmp_path[512];
        if (fetch_and_save_image_from_url(image_url, saved_bmp_path, sizeof(saved_bmp_path)) ==
            ESP_OK) {
            ESP_LOGI(TAG, "Successfully downloaded and saved image, displaying...");
            display_manager_show_image(saved_bmp_path);

            // Delete rendered temp image after display to save storage space,
            // but only if it wasn't saved to the Downloads album.
            if (!config_manager_get_save_downloaded_images()) {
                unlink(CURRENT_BMP_PATH);
                unlink(CURRENT_PNG_PATH);
            }

            result = ESP_OK;
        } else {
            ESP_LOGE(TAG, "Failed to download image from URL, falling back to local rotation");
            display_manager_rotate_from_storage(skip_count);
            result = ESP_FAIL;
        }
    } else {
        // Local storage mode - rotate through albums
        display_manager_rotate_from_storage(skip_count);
        result = ESP_OK;
    }

    if (result == ESP_OK) {
        config_manager_set_last_rotation_timestamp(time(NULL));
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
    time(&now);

    time_t last_rot = (time_t) config_manager_get_last_rotation_timestamp();
    rotation_config_t rot_config = {
        .mode = config_manager_get_ar_mode(),
        .interval_sec = config_manager_get_rotate_interval(),
        .start_time_min = config_manager_get_ar_start_time(),
        .use_anchor = config_manager_get_auto_rotate_aligned(),
        .policy = config_manager_get_ar_sleep_policy(),
        .last_rotation = last_rot,
    };

    sleep_schedule_config_t sleep_schedule = {
        .enabled = config_manager_get_sleep_schedule_enabled(),
        .start_minutes = config_manager_get_sleep_schedule_start(),
        .end_minutes = config_manager_get_sleep_schedule_end(),
    };

    return calculate_next_wakeup_interval(now, &rot_config, &sleep_schedule);
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
