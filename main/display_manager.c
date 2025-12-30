#include "display_manager.h"

#include <dirent.h>
#include <string.h>
#include <sys/stat.h>

#include "GUI_BMPfile.h"
#include "GUI_Paint.h"
#include "album_manager.h"
#include "config.h"
#include "epaper_port.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "power_manager.h"

static const char *TAG = "display_manager";

static SemaphoreHandle_t display_mutex = NULL;
static int rotate_interval = IMAGE_ROTATE_INTERVAL_SEC;
static bool auto_rotate_enabled = false;
static char current_image[64] = {0};

static uint8_t *epd_image_buffer = NULL;
static uint32_t image_buffer_size;

esp_err_t display_manager_init(void)
{
    display_mutex = xSemaphoreCreateMutex();
    if (!display_mutex) {
        ESP_LOGE(TAG, "Failed to create display mutex");
        return ESP_FAIL;
    }

    epaper_port_init();

    image_buffer_size =
        ((DISPLAY_WIDTH % 2 == 0) ? (DISPLAY_WIDTH / 2) : (DISPLAY_WIDTH / 2 + 1)) * DISPLAY_HEIGHT;
    epd_image_buffer = (uint8_t *) heap_caps_malloc(image_buffer_size, MALLOC_CAP_SPIRAM);
    if (!epd_image_buffer) {
        ESP_LOGE(TAG, "Failed to allocate image buffer");
        return ESP_FAIL;
    }

    Paint_NewImage(epd_image_buffer, DISPLAY_WIDTH, DISPLAY_HEIGHT, 0, EPD_7IN3E_WHITE);
    Paint_SetScale(6);
    Paint_SelectImage(epd_image_buffer);
    Paint_SetRotate(180);

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        int32_t stored_interval = IMAGE_ROTATE_INTERVAL_SEC;
        if (nvs_get_i32(nvs_handle, NVS_ROTATE_INTERVAL_KEY, &stored_interval) == ESP_OK) {
            rotate_interval = stored_interval;
            ESP_LOGI(TAG, "Loaded rotate interval from NVS: %d seconds", rotate_interval);
        }

        uint8_t stored_enabled = 0;
        if (nvs_get_u8(nvs_handle, NVS_AUTO_ROTATE_KEY, &stored_enabled) == ESP_OK) {
            auto_rotate_enabled = (stored_enabled != 0);
            ESP_LOGI(TAG, "Loaded auto-rotate enabled from NVS: %s",
                     auto_rotate_enabled ? "yes" : "no");
        }

        // Random rotation - no need to load index from NVS

        nvs_close(nvs_handle);
    }

    nvs_handle_t nvs_handle2;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle2) == ESP_OK) {
        size_t len = sizeof(current_image);
        nvs_get_str(nvs_handle2, NVS_CURRENT_IMAGE_KEY, current_image, &len);

        nvs_close(nvs_handle2);
    }

    ESP_LOGI(TAG, "Display manager initialized");
    ESP_LOGI(TAG, "Auto-rotate uses timer-based wake-up (only works during sleep cycles)");
    return ESP_OK;
}

esp_err_t display_manager_show_image(const char *filename)
{
    if (!filename || strlen(filename) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(display_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire display mutex");
        return ESP_FAIL;
    }

    // Expect absolute path from caller
    ESP_LOGI(TAG, "Displaying image: %s", filename);
    ESP_LOGI(TAG, "Free heap before display: %lu bytes", esp_get_free_heap_size());

    ESP_LOGI(TAG, "Clearing display buffer");
    Paint_Clear(EPD_7IN3E_WHITE);

    ESP_LOGI(TAG, "Reading BMP file into buffer");
    if (GUI_ReadBmp_RGB_6Color(filename, 0, 0) != 0) {
        ESP_LOGE(TAG, "Failed to read BMP file");
        xSemaphoreGive(display_mutex);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Starting e-paper display update (this takes ~30 seconds)");
    ESP_LOGI(TAG, "Free heap before epaper_port_display: %lu bytes", esp_get_free_heap_size());

    // Yield to watchdog before long operation
    vTaskDelay(pdMS_TO_TICKS(10));

    ESP_LOGI(TAG, "Calling epaper_port_display...");
    epaper_port_display(epd_image_buffer);
    ESP_LOGI(TAG, "epaper_port_display returned successfully");

    ESP_LOGI(TAG, "E-paper display update complete");
    ESP_LOGI(TAG, "Free heap after display: %lu bytes", esp_get_free_heap_size());

    strncpy(current_image, filename, sizeof(current_image) - 1);

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_str(nvs_handle, NVS_CURRENT_IMAGE_KEY, current_image);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    xSemaphoreGive(display_mutex);

    ESP_LOGI(TAG, "Image displayed successfully");
    return ESP_OK;
}

esp_err_t display_manager_clear(void)
{
    if (xSemaphoreTake(display_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_FAIL;
    }

    epaper_port_clear(epd_image_buffer, EPD_7IN3E_WHITE);
    epaper_port_display(epd_image_buffer);

    xSemaphoreGive(display_mutex);
    return ESP_OK;
}

bool display_manager_is_busy(void)
{
    // Try to take the mutex without blocking
    if (xSemaphoreTake(display_mutex, 0) == pdTRUE) {
        // Mutex was available, give it back
        xSemaphoreGive(display_mutex);
        return false;
    }
    // Mutex is held by another task
    return true;
}

void display_manager_set_rotate_interval(int seconds)
{
    rotate_interval = seconds;

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_i32(nvs_handle, NVS_ROTATE_INTERVAL_KEY, seconds);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Rotate interval set to %d seconds", seconds);
}

int display_manager_get_rotate_interval(void)
{
    return rotate_interval;
}

void display_manager_set_auto_rotate(bool enabled)
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

bool display_manager_get_auto_rotate(void)
{
    return auto_rotate_enabled;
}

void display_manager_handle_wakeup(void)
{
    if (!auto_rotate_enabled) {
        ESP_LOGI(TAG, "Manual rotation triggered (auto-rotate is disabled)");
    } else {
        ESP_LOGI(TAG, "Handling wakeup for auto-rotate");
    }

    // Get enabled albums
    char **enabled_albums = NULL;
    int album_count = 0;
    if (album_manager_get_enabled_albums(&enabled_albums, &album_count) != ESP_OK ||
        album_count == 0) {
        ESP_LOGW(TAG, "No enabled albums for auto-rotate");
        return;
    }

    ESP_LOGI(TAG, "Collecting images from %d enabled album(s)", album_count);

    // Check for stale albums (removed from SD card) and disable them
    bool found_stale_albums = false;
    for (int i = 0; i < album_count; i++) {
        if (!album_manager_album_exists(enabled_albums[i])) {
            ESP_LOGW(TAG, "Album '%s' no longer exists on SD card, disabling it",
                     enabled_albums[i]);
            album_manager_set_album_enabled(enabled_albums[i], false);
            found_stale_albums = true;
        }
    }

    // If we found stale albums, reload the enabled list
    if (found_stale_albums) {
        album_manager_free_album_list(enabled_albums, album_count);
        if (album_manager_get_enabled_albums(&enabled_albums, &album_count) != ESP_OK ||
            album_count == 0) {
            ESP_LOGW(TAG, "No enabled albums remaining after cleanup");
            return;
        }
        ESP_LOGI(TAG, "After cleanup: %d enabled album(s)", album_count);
    }

    // Count total images across all enabled albums
    int total_image_count = 0;
    for (int i = 0; i < album_count; i++) {
        char album_path[256];
        album_manager_get_album_path(enabled_albums[i], album_path, sizeof(album_path));

        DIR *dir = opendir(album_path);
        if (!dir) {
            ESP_LOGW(TAG, "Failed to open album: %s", enabled_albums[i]);
            continue;
        }

        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_type == DT_REG) {
                if (entry->d_name[0] == '.' && entry->d_name[1] == '_') {
                    continue;
                }
                const char *ext = strrchr(entry->d_name, '.');
                if (ext && (strcmp(ext, ".bmp") == 0 || strcmp(ext, ".BMP") == 0)) {
                    total_image_count++;
                }
            }
        }
        closedir(dir);
    }

    if (total_image_count == 0) {
        ESP_LOGW(TAG, "No images found in enabled albums");
        album_manager_free_album_list(enabled_albums, album_count);
        return;
    }

    // Build image list with absolute paths from all enabled albums
    char **image_list = malloc(total_image_count * sizeof(char *));
    int idx = 0;

    for (int i = 0; i < album_count; i++) {
        char album_path[256];
        album_manager_get_album_path(enabled_albums[i], album_path, sizeof(album_path));

        DIR *dir = opendir(album_path);
        if (!dir) {
            continue;
        }

        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL && idx < total_image_count) {
            if (entry->d_type == DT_REG) {
                if (entry->d_name[0] == '.' && entry->d_name[1] == '_') {
                    continue;
                }

                const char *ext = strrchr(entry->d_name, '.');
                if (ext && (strcmp(ext, ".bmp") == 0 || strcmp(ext, ".BMP") == 0)) {
                    char *fullpath = malloc(512);
                    snprintf(fullpath, 512, "%s/%s", album_path, entry->d_name);
                    image_list[idx] = fullpath;
                    idx++;
                }
            }
        }
        closedir(dir);
    }

    album_manager_free_album_list(enabled_albums, album_count);

    // Select random image
    int random_index = esp_random() % total_image_count;

    // Display random image
    ESP_LOGI(TAG, "Auto-rotate: Displaying random image %d/%d: %s", random_index + 1,
             total_image_count, image_list[random_index]);
    display_manager_show_image(image_list[random_index]);

    // Free image list
    for (int i = 0; i < total_image_count; i++) {
        free(image_list[i]);
    }
    free(image_list);

    ESP_LOGI(TAG, "Auto-rotate complete");
}
