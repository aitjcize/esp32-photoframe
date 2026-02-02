#include "display_manager.h"

#include <dirent.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "GUI_BMPfile.h"
#include "GUI_PNGfile.h"
#include "GUI_Paint.h"
#include "album_manager.h"
#include "board_hal.h"
#include "config.h"
#include "config_manager.h"
#include "epaper_port.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

static const char *TAG = "display_manager";
#define NVS_LAST_IMAGE_KEY "last_image"

static SemaphoreHandle_t display_mutex = NULL;
static char current_image[64] = {0};
static char last_displayed_image[256] = {0};  // Internal state: last displayed image path

static uint8_t *epd_image_buffer = NULL;
static uint32_t image_buffer_size;

// Load last displayed image from NVS
static void load_last_displayed_image(void)
{
    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle) == ESP_OK) {
        size_t len = sizeof(last_displayed_image);
        if (nvs_get_str(nvs_handle, NVS_LAST_IMAGE_KEY, last_displayed_image, &len) == ESP_OK) {
            ESP_LOGI(TAG, "Loaded last displayed image: %s", last_displayed_image);
        } else {
            last_displayed_image[0] = '\0';
        }
        nvs_close(nvs_handle);
    }
}

// Save last displayed image to NVS
static void save_last_displayed_image(const char *filename)
{
    if (filename == NULL) {
        return;
    }

    strncpy(last_displayed_image, filename, sizeof(last_displayed_image) - 1);
    last_displayed_image[sizeof(last_displayed_image) - 1] = '\0';

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_str(nvs_handle, NVS_LAST_IMAGE_KEY, last_displayed_image);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    ESP_LOGI(TAG, "Saved last displayed image: %s", last_displayed_image);
}

// Helper function to create link file pointing to current image
static void create_image_link(const char *target_path)
{
    FILE *fp = fopen(CURRENT_IMAGE_LINK, "w");
    if (fp) {
        fprintf(fp, "%s", target_path);
        fclose(fp);
        ESP_LOGD(TAG, "Created link file pointing to: %s", target_path);
    } else {
        ESP_LOGE(TAG, "Failed to create link file");
    }
}

esp_err_t display_manager_init(void)
{
    display_mutex = xSemaphoreCreateMutex();
    if (!display_mutex) {
        ESP_LOGE(TAG, "Failed to create display mutex");
        return ESP_FAIL;
    }

    // epaper_port_init() is now called by board_hal_init()

    image_buffer_size = ((BOARD_HAL_DISPLAY_WIDTH % 2 == 0) ? (BOARD_HAL_DISPLAY_WIDTH / 2)
                                                            : (BOARD_HAL_DISPLAY_WIDTH / 2 + 1)) *
                        BOARD_HAL_DISPLAY_HEIGHT;
    epd_image_buffer = (uint8_t *) heap_caps_malloc(image_buffer_size, MALLOC_CAP_SPIRAM);
    if (!epd_image_buffer) {
        ESP_LOGE(TAG, "Failed to allocate image buffer");
        return ESP_FAIL;
    }

    display_manager_initialize_paint();

    ESP_LOGI(TAG, "Display manager initialized");
    ESP_LOGI(TAG, "Auto-rotate uses timer-based wake-up (only works during sleep cycles)");
    return ESP_OK;
}

void display_manager_initialize_paint(void)
{
    Paint_NewImage(epd_image_buffer, BOARD_HAL_DISPLAY_WIDTH, BOARD_HAL_DISPLAY_HEIGHT,
                   config_manager_get_display_rotation_deg() % 360, EPD_7IN3E_WHITE);
    Paint_SetScale(6);
    Paint_SelectImage(epd_image_buffer);
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

    // Detect file type by extension
    const char *ext = strrchr(filename, '.');
    bool is_png = (ext != NULL && strcasecmp(ext, ".png") == 0);

    if (is_png) {
        ESP_LOGI(TAG, "Reading PNG file into buffer");
        if (GUI_ReadPng_RGB_6Color(filename, 0, 0) != 0) {
            ESP_LOGE(TAG, "Failed to read PNG file");
            xSemaphoreGive(display_mutex);
            return ESP_FAIL;
        }
    } else {
        ESP_LOGI(TAG, "Reading BMP file into buffer");
        if (GUI_ReadBmp_RGB_6Color(filename, 0, 0) != 0) {
            ESP_LOGE(TAG, "Failed to read BMP file");
            xSemaphoreGive(display_mutex);
            return ESP_FAIL;
        }
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

    create_image_link(filename);
    ESP_LOGD(TAG, "Created link to: %s", filename);

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

    // Remove the current image link so API returns 404
    unlink(CURRENT_IMAGE_LINK);
    current_image[0] = '\0';
    save_last_displayed_image("");

    xSemaphoreGive(display_mutex);
    return ESP_OK;
}

esp_err_t display_manager_show_calibration(void)
{
    if (xSemaphoreTake(display_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire display mutex for calibration");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Displaying calibration pattern");

    // Re-initialize paint with current orientation
    display_manager_initialize_paint();

    // Draw the calibration pattern directly to the buffer
    Paint_DrawCalibrationPattern();

    // Display the buffer
    epaper_port_display(epd_image_buffer);

    xSemaphoreGive(display_mutex);

    ESP_LOGI(TAG, "Calibration pattern displayed successfully");
    return ESP_OK;
}

esp_err_t display_manager_show_setup_screen(void)
{
    if (xSemaphoreTake(display_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire display mutex for setup screen");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Displaying setup screen");

    // Re-initialize paint with current orientation
    display_manager_initialize_paint();

    // Display a purple background with simple checkboard dithering pattern
    uint16_t width = Paint.Width;
    uint16_t height = Paint.Height;

    for (uint16_t y = 0; y < height; y++) {
        for (uint16_t x = 0; x < width; x++) {
            // Checkerboard
            if ((x + y) % 2 == 0) {
                Paint_SetPixel(x, y, EPD_7IN3E_RED);
            } else {
                Paint_SetPixel(x, y, EPD_7IN3E_BLUE);
            }
        }
    }

    // 2. Draw Text
    sFONT *font = &Font24;

    const char *body_lines[] = {"", "Setup required!", "1. Connect WiFi: PhotoFrame-Setup  ",
                                "2. Visit URL   : http://192.168.4.1", NULL};

    // Calculate longest body line dynamically
    size_t longest_body_len = 0;
    int num_body_lines = 0;
    while (body_lines[num_body_lines]) {
        size_t len = strlen(body_lines[num_body_lines]);
        if (len > longest_body_len) {
            longest_body_len = len;
        }
        num_body_lines++;
    }

    // Scaling Logic: Best fit for Body Text
    // We check against width with some padding
    uint8_t body_scale = 4;

    while (body_scale > 1) {
        uint16_t required_width = longest_body_len * font->Width * body_scale + (40 * body_scale);
        if (required_width < width) {
            break;
        }
        body_scale--;
    }

    // Title Scale: Try 2x body scale, but ensure it fits
    const char *title_text = "ESP32-PhotoFrame";
    uint8_t title_scale = body_scale * 2;
    size_t title_len = strlen(title_text);

    while (title_scale > body_scale) {  // Don't go smaller than body scale ideally
        uint16_t required_width = title_len * font->Width * title_scale + (20 * title_scale);
        if (required_width < width) {
            break;
        }
        title_scale--;
    }

    ESP_LOGI(TAG, "Setup screen scale - Title: %d, Body: %d", title_scale, body_scale);

    // Calculate Layout Heights
    uint16_t title_height = font->Height * title_scale;
    uint16_t body_line_height = font->Height * body_scale;
    uint16_t padding = 20 * body_scale;  // Base padding

    uint16_t total_content_height = title_height + padding + (num_body_lines * body_line_height) +
                                    ((num_body_lines - 1) * padding);

    uint16_t start_y = (total_content_height < height) ? (height - total_content_height) / 2 : 0;
    uint16_t current_y = start_y;

    // Draw Title
    uint16_t title_width = title_len * font->Width * title_scale;
    uint16_t title_x = (width > title_width) ? (width - title_width) / 2 : 0;

    Paint_DrawString_EN_Scaled(title_x, current_y, title_text, font, EPD_7IN3E_WHITE, WHITE,
                               title_scale, true);

    current_y += title_height + padding;

    // Draw Body Lines
    for (int i = 0; i < num_body_lines; i++) {
        const char *text = body_lines[i];
        size_t len = strlen(text);
        uint16_t text_width = len * font->Width * body_scale;

        uint16_t text_x = (width > text_width) ? (width - text_width) / 2 : 0;

        Paint_DrawString_EN_Scaled(text_x, current_y, text, font, EPD_7IN3E_WHITE, WHITE,
                                   body_scale, true);

        current_y += body_line_height + padding;
    }

    // Display the buffer
    epaper_port_display(epd_image_buffer);

    xSemaphoreGive(display_mutex);

    ESP_LOGI(TAG, "Setup screen displayed successfully");
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

const char *display_manager_get_current_image(void)
{
    return current_image;
}

static void rotate_sequential(char **enabled_albums, int album_count)
{
    ESP_LOGI(TAG, "Sequential rotation mode");
    int32_t last_idx = config_manager_get_last_index();
    int32_t target_idx = last_idx + 1;
    int32_t current_idx = 0;
    char first_image[512] = {0};
    bool found_target = false;

    for (int i = 0; i < album_count; i++) {
        char album_path[256];
        album_manager_get_album_path(enabled_albums[i], album_path, sizeof(album_path));

        DIR *dir = opendir(album_path);
        if (!dir) {
            continue;
        }

        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_type == DT_REG) {
                if (entry->d_name[0] == '.' && entry->d_name[1] == '_') {
                    continue;
                }

                const char *ext = strrchr(entry->d_name, '.');
                if (ext && (strcmp(ext, ".bmp") == 0 || strcmp(ext, ".BMP") == 0 ||
                            strcmp(ext, ".png") == 0 || strcmp(ext, ".PNG") == 0)) {
                    char fullpath[512];
                    snprintf(fullpath, sizeof(fullpath), "%s/%s", album_path, entry->d_name);

                    // Keep track of the very first image in case we need to wrap
                    if (first_image[0] == '\0') {
                        strncpy(first_image, fullpath, sizeof(first_image) - 1);
                    }

                    if (current_idx == target_idx) {
                        ESP_LOGI(TAG, "Found target index %ld: %s", (long) target_idx, fullpath);
                        display_manager_show_image(fullpath);
                        save_last_displayed_image(fullpath);
                        config_manager_set_last_index(target_idx);
                        found_target = true;
                        closedir(dir);
                        return;
                    }
                    current_idx++;
                }
            }
        }
        closedir(dir);
    }

    // If we reached here, we didn't find the target index (or the list has changed and is
    // shorter) Wrap around to the first image
    if (!found_target) {
        if (first_image[0] != '\0') {
            ESP_LOGI(TAG, "Wrapping around to start. Displaying: %s", first_image);
            display_manager_show_image(first_image);
            save_last_displayed_image(first_image);
            config_manager_set_last_index(0);  // Reset index to 0
        } else {
            ESP_LOGW(TAG, "No images found in any enabled albums.");
        }
    }
}

static void rotate_random(char **enabled_albums, int album_count)
{
    ESP_LOGI(TAG, "Random rotation mode");

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
                if (ext && (strcmp(ext, ".bmp") == 0 || strcmp(ext, ".BMP") == 0 ||
                            strcmp(ext, ".png") == 0 || strcmp(ext, ".PNG") == 0)) {
                    total_image_count++;
                }
            }
        }
        closedir(dir);
    }

    if (total_image_count == 0) {
        ESP_LOGW(TAG, "No images found in enabled albums");
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
                if (ext && (strcmp(ext, ".bmp") == 0 || strcmp(ext, ".BMP") == 0 ||
                            strcmp(ext, ".png") == 0 || strcmp(ext, ".PNG") == 0)) {
                    char *fullpath = malloc(512);
                    snprintf(fullpath, 512, "%s/%s", album_path, entry->d_name);
                    image_list[idx] = fullpath;
                    idx++;
                }
            }
        }
        closedir(dir);
    }

    // Load last displayed image if not already loaded
    if (last_displayed_image[0] == '\0') {
        load_last_displayed_image();
    }

    // Select random image, avoiding the last displayed image if possible
    int random_index = esp_random() % total_image_count;

    // If we have more than one image and the random selection matches the last image,
    // try to pick a different one (up to 10 attempts)
    if (total_image_count > 1 && last_displayed_image[0] != '\0') {
        int attempts = 0;
        while (attempts < 10 && strcmp(image_list[random_index], last_displayed_image) == 0) {
            random_index = esp_random() % total_image_count;
            attempts++;
        }

        if (strcmp(image_list[random_index], last_displayed_image) == 0) {
            ESP_LOGW(TAG, "Could not avoid repeating last image after 10 attempts");
        } else {
            ESP_LOGI(TAG, "Successfully avoided repeating last image");
        }
    }

    // Display random image
    ESP_LOGI(TAG, "Auto-rotate: Displaying random image %d/%d: %s", random_index + 1,
             total_image_count, image_list[random_index]);
    display_manager_show_image(image_list[random_index]);

    // Store the displayed image filename in NVS
    save_last_displayed_image(image_list[random_index]);

    // Free image list
    for (int i = 0; i < total_image_count; i++) {
        free(image_list[i]);
    }
    free(image_list);
}

void display_manager_rotate_from_sdcard(void)
{
    if (!config_manager_get_auto_rotate()) {
        ESP_LOGI(TAG, "Manual rotation triggered (auto-rotate is disabled)");
    } else {
        ESP_LOGI(TAG, "Rotating from SD card");
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

    // Get rotation mode
    sd_rotation_mode_t mode = config_manager_get_sd_rotation_mode();

    if (mode == SD_ROTATION_SEQUENTIAL) {
        rotate_sequential(enabled_albums, album_count);
    } else {
        rotate_random(enabled_albums, album_count);
    }

    album_manager_free_album_list(enabled_albums, album_count);
    ESP_LOGI(TAG, "Auto-rotate complete");
}
