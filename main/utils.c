#include "utils.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "axp_prot.h"
#include "cJSON.h"
#include "config.h"
#include "config_manager.h"
#include "display_manager.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "image_processor.h"

static const char *TAG = "utils";

// Context for HTTP event handler
typedef struct {
    FILE *file;
    int total_read;
    char *content_type;
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
        if (strcmp(evt->header_key, "Content-Type") == 0) {
            sprintf(ctx->content_type, "%s", evt->header_value);
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

esp_err_t fetch_and_save_image_from_url(const char *url, char *saved_bmp_path, size_t path_size)
{
    ESP_LOGI(TAG, "Fetching image from URL: %s", url);

    // Use fixed paths for current image and thumbnail
    const char *temp_jpg_path = CURRENT_JPG_PATH;
    const char *temp_bmp_path = CURRENT_BMP_PATH;

    esp_err_t err = ESP_FAIL;
    int status_code = 0;
    int content_length = 0;
    char *content_type;
    int total_downloaded = 0;
    const int max_retries = 3;

    // Retry loop
    for (int retry = 0; retry < max_retries; retry++) {
        if (retry > 0) {
            ESP_LOGW(TAG, "Retry attempt %d/%d after 2 second delay...", retry + 1, max_retries);
            vTaskDelay(pdMS_TO_TICKS(2000));  // 2 second delay between retries
        }

        FILE *file = fopen(temp_jpg_path, "wb");
        if (!file) {
            ESP_LOGE(TAG, "Failed to open file for writing: %s", temp_jpg_path);
            continue;  // Try again
        }

        download_context_t ctx = {
            .file = file,
            .total_read = 0,
            .content_type = malloc(128)
        };

        esp_http_client_config_t config = {
            .url = url,
            .timeout_ms = 30000,
            .event_handler = http_event_handler,
            .user_data = &ctx,
            .max_redirection_count = 5,
            .user_agent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36",
        };

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) {
            ESP_LOGE(TAG, "Failed to initialize HTTP client");
            fclose(file);
            continue;  // Try again
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
            ESP_LOGI(TAG, "Downloaded %d bytes (content_length: %d), converting JPG to BMP...",
                     total_downloaded, content_length);
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

        // Clean up failed download
        unlink(temp_jpg_path);
    }

    // Check final result after all retries
    if (err != ESP_OK || status_code != 200 || total_downloaded <= 0) {
        ESP_LOGE(TAG, "Failed to download image after %d attempts", max_retries);
        unlink(temp_jpg_path);
        return ESP_FAIL;
    }

    // Convert JPG to BMP using image processor
    if (strcmp(content_type, "image/bmp") == 0) {
        // just move our temp "jpg" to the bmp file
        unlink(temp_bmp_path);
        if (rename(temp_jpg_path, temp_bmp_path) != 0) {
            ESP_LOGE(TAG, "Failed to move downloaded BMP to temp location");
            unlink(temp_jpg_path);
            unlink(temp_bmp_path);
            return ESP_FAIL;
        }
    } else {
        err = image_processor_convert_jpg_to_bmp(temp_jpg_path, temp_bmp_path, false);
    }
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to convert JPG to BMP: %s", esp_err_to_name(err));
        unlink(temp_jpg_path);

        // Provide specific error messages based on error type
        if (err == ESP_ERR_INVALID_SIZE) {
            ESP_LOGE(TAG, "Image is too large (max: 6400x3840)");
        } else if (err == ESP_ERR_NO_MEM) {
            ESP_LOGE(TAG, "Image requires too much memory to process");
        }
        return err;
    }

    ESP_LOGI(TAG, "Successfully converted image to BMP");

    // Check if we should save downloaded images
    bool save_images = config_manager_get_save_downloaded_images();

    if (save_images) {
        // Save to Downloads album
        char downloads_path[256];
        snprintf(downloads_path, sizeof(downloads_path), "%s/Downloads", IMAGE_DIRECTORY);

        // Create Downloads directory if it doesn't exist
        struct stat st;
        if (stat(downloads_path, &st) != 0) {
            ESP_LOGI(TAG, "Creating Downloads album directory");
            if (mkdir(downloads_path, 0755) != 0) {
                ESP_LOGE(TAG, "Failed to create Downloads directory");
                unlink(temp_jpg_path);
                unlink(temp_bmp_path);
                return ESP_FAIL;
            }
        }

        // Generate unique filename based on timestamp
        time_t now = time(NULL);
        char filename_base[64];
        snprintf(filename_base, sizeof(filename_base), "download_%lld", (long long) now);

        // Save BMP to Downloads album
        char final_bmp_path[512];
        snprintf(final_bmp_path, sizeof(final_bmp_path), "%s/%s.bmp", downloads_path,
                 filename_base);

        if (rename(temp_bmp_path, final_bmp_path) != 0) {
            ESP_LOGE(TAG, "Failed to move BMP to Downloads album");
            unlink(temp_jpg_path);
            unlink(temp_bmp_path);
            return ESP_FAIL;
        }

        // Save JPG thumbnail to Downloads album (using original downloaded JPG as thumbnail)
        char final_jpg_path[512];
        snprintf(final_jpg_path, sizeof(final_jpg_path), "%s/%s.jpg", downloads_path,
                 filename_base);

        if (rename(temp_jpg_path, final_jpg_path) != 0) {
            ESP_LOGE(TAG, "Failed to move JPG thumbnail to Downloads album");
            unlink(temp_jpg_path);
            // BMP is already moved, keep it
            return ESP_FAIL;
        }

        ESP_LOGI(TAG, "Saved to Downloads album: %s.bmp (with thumbnail)", filename_base);

        // Return the saved BMP path via output parameter
        snprintf(saved_bmp_path, path_size, "%s", final_bmp_path);
    } else {
        // Just use temp BMP path without saving to album
        ESP_LOGI(TAG, "Displaying image without saving (save_downloaded_images disabled)");
        snprintf(saved_bmp_path, path_size, "%s", temp_bmp_path);

        // Keep both .current.bmp and .current.jpg for HA integration
        // .current.bmp is referenced by current_image
        // .current.jpg is the thumbnail served by /api/current_image
        ESP_LOGI(TAG, "Image and thumbnail saved: %s, %s", temp_bmp_path, temp_jpg_path);
    }

    return ESP_OK;
}

esp_err_t trigger_image_rotation(void)
{
    rotation_mode_t rotation_mode = config_manager_get_rotation_mode();

    if (rotation_mode == ROTATION_MODE_URL) {
        // URL mode - fetch image from URL
        const char *image_url = config_manager_get_image_url();
        ESP_LOGI(TAG, "URL rotation mode - downloading from: %s", image_url);

        char saved_bmp_path[512];
        if (fetch_and_save_image_from_url(image_url, saved_bmp_path, sizeof(saved_bmp_path)) ==
            ESP_OK) {
            ESP_LOGI(TAG, "Successfully downloaded and saved image, displaying...");
            display_manager_show_image(saved_bmp_path);
            return ESP_OK;
        } else {
            ESP_LOGE(TAG, "Failed to download image from URL, falling back to SD card rotation");
            display_manager_rotate_from_sdcard();
            return ESP_FAIL;
        }
    } else {
        // SD card mode - rotate through albums
        display_manager_rotate_from_sdcard();
        return ESP_OK;
    }
}

cJSON *create_battery_json(void)
{
    cJSON *json = cJSON_CreateObject();
    if (json == NULL) {
        return NULL;
    }

    int battery_percent = axp_get_battery_percent();
    int battery_voltage = axp_get_battery_voltage();
    bool is_charging = axp_is_charging();
    bool usb_connected = axp_is_usb_connected();
    bool battery_connected = axp_is_battery_connected();

    cJSON_AddNumberToObject(json, "battery_level", battery_percent);
    cJSON_AddNumberToObject(json, "battery_voltage", battery_voltage);
    cJSON_AddBoolToObject(json, "charging", is_charging);
    cJSON_AddBoolToObject(json, "usb_connected", usb_connected);
    cJSON_AddBoolToObject(json, "battery_connected", battery_connected);

    return json;
}
