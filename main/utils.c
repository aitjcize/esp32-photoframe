#include "utils.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "ai_manager.h"
#include "board_hal.h"
#include "cJSON.h"
#include "color_palette.h"
#include "config.h"
#include "config_manager.h"
#include "display_manager.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "image_processor.h"
#include "processing_settings.h"
#ifdef CONFIG_HAS_SDCARD
#include "sdcard.h"
#endif
#include "testable_utils.h"

static const char *TAG = "utils";

// Context for HTTP event handler
typedef struct {
    FILE *file;
    int total_read;
    char *content_type;
    char *thumbnail_url;  // Optional thumbnail URL from X-Thumbnail-URL header
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
            sprintf(ctx->content_type, "%s", evt->header_value);
        } else if (strcasecmp(evt->header_key, "X-Thumbnail-URL") == 0) {
            // Capture thumbnail URL if provided by server (case-insensitive)
            if (ctx->thumbnail_url && strlen(evt->header_value) > 0) {
                strncpy(ctx->thumbnail_url, evt->header_value, 511);
                ctx->thumbnail_url[511] = '\0';
                ESP_LOGI(TAG, "Thumbnail URL provided: %s", ctx->thumbnail_url);
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

    // Allocate buffers once before retry loop
    thumbnail_url_buffer = calloc(512, 1);
    content_type = calloc(128, 1);

    if (!content_type || !thumbnail_url_buffer) {
        ESP_LOGE(TAG, "Failed to allocate memory for download context");
        if (content_type)
            free(content_type);
        if (thumbnail_url_buffer)
            free(thumbnail_url_buffer);
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

        download_context_t ctx = {.file = file,
                                  .total_read = 0,
                                  .content_type = content_type,
                                  .thumbnail_url = thumbnail_url_buffer};

        esp_http_client_config_t config = {
            .url = url,
            .timeout_ms = 30000,
            .event_handler = http_event_handler,
            .user_data = &ctx,
            .max_redirection_count = 5,
            .user_agent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36",
            .buffer_size_tx = 2048,
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
        free(content_type);
        free(thumbnail_url_buffer);
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

    const char *final_path = NULL;

    // ========== STEP 1: Image Processing (always done first) ==========
    if (image_format == IMAGE_FORMAT_BMP) {
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
            bool use_stock_mode = (strcmp(settings.processing_mode, "stock") == 0);
            dither_algorithm_t algo = processing_settings_get_dithering_algorithm();

            err = image_processor_process(temp_upload_path, temp_png_path, use_stock_mode, algo);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to process image: %s", esp_err_to_name(err));
                unlink(temp_upload_path);
                return err;
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
#ifdef CONFIG_HAS_SDCARD
    bool save_images = config_manager_get_save_downloaded_images();

    if (save_images && !sdcard_is_mounted()) {
        ESP_LOGI(TAG, "SD card not mounted, skipping save to Downloads");
        save_images = false;
    }

    if (save_images) {
        char downloads_path[256];
        snprintf(downloads_path, sizeof(downloads_path), "%s/Downloads", IMAGE_DIRECTORY);

        // Create Downloads directory if it doesn't exist
        struct stat st;
        if (stat(downloads_path, &st) != 0) {
            ESP_LOGI(TAG, "Creating Downloads album directory");
            if (mkdir(downloads_path, 0755) != 0) {
                ESP_LOGE(TAG, "Failed to create Downloads directory");
                // Processing succeeded, just can't save to album - fall through to use temp path
                goto use_temp_path;
            }
        }

        // Generate unique filename based on timestamp
        time_t now = time(NULL);
        char filename_base[64];
        snprintf(filename_base, sizeof(filename_base), "download_%lld", (long long) now);

        char final_image_path[512];
        bool thumbnail_saved_to_album = false;

        if (image_format == IMAGE_FORMAT_BMP) {
            snprintf(final_image_path, sizeof(final_image_path), "%s/%s.bmp", downloads_path,
                     filename_base);
            if (rename(final_path, final_image_path) != 0) {
                ESP_LOGW(TAG, "Failed to move BMP to Downloads album, using temp path");
            } else {
                final_path = NULL;  // Will be set below
            }
        } else {
            snprintf(final_image_path, sizeof(final_image_path), "%s/%s.png", downloads_path,
                     filename_base);
            if (rename(final_path, final_image_path) != 0) {
                ESP_LOGW(TAG, "Failed to move PNG to Downloads album, using temp path");
            } else {
                final_path = NULL;  // Will be set below
            }
        }

        // Move thumbnail to album if we successfully moved the main image
        if (final_path == NULL) {
            struct stat thumb_st;
            if (stat(temp_jpg_path, &thumb_st) == 0) {
                char final_thumb_path[512];
                snprintf(final_thumb_path, sizeof(final_thumb_path), "%s/%s.jpg", downloads_path,
                         filename_base);
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
#else
    // No SD card support - just use temp path
    snprintf(saved_image_path, path_size, "%s", final_path);
    ESP_LOGI(TAG, "Image processed: %s", final_path);
    if (thumbnail_downloaded) {
        ESP_LOGI(TAG, "Downloaded thumbnail available: %s", temp_jpg_path);
    }
#endif

    ESP_LOGI(TAG, "Successfully processed image: %s", saved_image_path);

    return ESP_OK;
}

static esp_err_t perform_ai_rotation(void)
{
    const char *base_prompt = config_manager_get_ai_prompt();
    char prompt[AI_PROMPT_MAX_LEN];

    // Add random seed to prompt to ensure variation
    uint32_t seed = esp_random();
    snprintf(prompt, sizeof(prompt), "%s, seed: %lu",
             base_prompt ? base_prompt : "A random artistic image", (unsigned long) seed);

    ESP_LOGI(TAG, "AI rotation mode - generating with prompt: %s", prompt);

    if (ai_manager_generate(prompt) != ESP_OK) {
        return ESP_FAIL;
    }

    // Wait for completion (max 180 seconds)
    int timeout = 180;
    while (timeout > 0) {
        ai_generation_status_t status = ai_manager_get_status();
        if (status == AI_STATUS_COMPLETE) {
            const char *path = ai_manager_get_last_image_path();
            ESP_LOGI(TAG, "AI Generation complete, displaying: %s", path);
            return display_manager_show_image(path);
        } else if (status == AI_STATUS_ERROR) {
            ESP_LOGE(TAG, "AI Generation failed: %s", ai_manager_get_last_error());
            return ESP_FAIL;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
        timeout--;
    }

    ESP_LOGE(TAG, "AI Generation timed out");
    return ESP_ERR_TIMEOUT;
}

esp_err_t trigger_image_rotation(void)
{
    rotation_mode_t rotation_mode = config_manager_get_rotation_mode();
    esp_err_t result = ESP_OK;

    if (rotation_mode == ROTATION_MODE_AI) {
        // AI Mode
        if (perform_ai_rotation() != ESP_OK) {
            result = ESP_FAIL;
        }

        if (result != ESP_OK) {
            // Fallback to SD card
#ifdef CONFIG_HAS_SDCARD
            ESP_LOGW(TAG, "Falling back to SD card rotation");
            display_manager_rotate_from_sdcard();
#endif
        }

    } else if (rotation_mode == ROTATION_MODE_URL) {
        // URL mode - fetch image from URL
        const char *image_url = config_manager_get_image_url();
        ESP_LOGI(TAG, "URL rotation mode - downloading from: %s", image_url);

        char saved_bmp_path[512];
        if (fetch_and_save_image_from_url(image_url, saved_bmp_path, sizeof(saved_bmp_path)) ==
            ESP_OK) {
            ESP_LOGI(TAG, "Successfully downloaded and saved image, displaying...");
            display_manager_show_image(saved_bmp_path);
            result = ESP_OK;
        } else {
#ifdef CONFIG_HAS_SDCARD
            ESP_LOGE(TAG, "Failed to download image from URL, falling back to SD card rotation");
            display_manager_rotate_from_sdcard();
#endif
            result = ESP_FAIL;
        }
#ifdef CONFIG_HAS_SDCARD
    } else {
        // SD card mode - rotate through albums
        display_manager_rotate_from_sdcard();
        result = ESP_OK;
#endif
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

    int rotate_interval = config_manager_get_rotate_interval();
    bool aligned = config_manager_get_auto_rotate_aligned();

    sleep_schedule_config_t sleep_schedule = {
        .enabled = config_manager_get_sleep_schedule_enabled(),
        .start_minutes = config_manager_get_sleep_schedule_start(),
        .end_minutes = config_manager_get_sleep_schedule_end(),
    };

    return calculate_next_wakeup_interval(&timeinfo, rotate_interval, aligned, &sleep_schedule);
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
