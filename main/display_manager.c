#include "display_manager.h"

#include <dirent.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "GUI_BMPfile.h"
#include "GUI_ColorMap.h"
#include "GUI_EPDGZfile.h"
#include "GUI_PNGfile.h"
#include "GUI_Paint.h"
#include "GUI_RawBuffer.h"
#include "album_manager.h"
#include "board_hal.h"
#include "config.h"
#include "config_manager.h"
#include "epaper.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "storage.h"
#include "utils.h"
#include "zlib.h"

static const char *TAG = "display_manager";
#define NVS_LAST_IMAGE_KEY "last_image"

// Display operations (streamed processing plus the panel refresh) can
// legitimately hold the display mutex for a minute or more; waiters queue
// for a matching window instead of failing spuriously.
#define DISPLAY_LOCK_TIMEOUT_MS (120 * 1000)

// Grayscale (gc*) panels take linear-intensity nibbles (0=black..15=white)
// rather than Spectra ink-color indices, so both the decode mapping and the
// "white" fill value depend on the display type.
static bool display_is_grayscale(void)
{
    return strncmp(BOARD_HAL_DISPLAY_TYPE, "gc", 2) == 0;
}

static UWORD display_white_color(void)
{
    return display_is_grayscale() ? 0xF : EPD_7IN3E_WHITE;
}

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
    return ESP_OK;
}

void display_manager_initialize_paint(void)
{
    Paint_NewImage(epd_image_buffer, BOARD_HAL_DISPLAY_WIDTH, BOARD_HAL_DISPLAY_HEIGHT,
                   config_manager_get_display_rotation_deg() % 360, display_white_color());
    Paint_SetScale(display_is_grayscale() ? 16 : 6);
    Paint_SelectImage(epd_image_buffer);
}

esp_err_t display_manager_show_image(const char *filename)
{
    if (!filename || strlen(filename) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(display_mutex, pdMS_TO_TICKS(DISPLAY_LOCK_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire display mutex");
        return ESP_FAIL;
    }

    // Expect absolute path from caller
    ESP_LOGI(TAG, "Displaying image: %s", filename);
    ESP_LOGI(TAG, "Free heap before display: %lu bytes", esp_get_free_heap_size());

    ESP_LOGI(TAG, "Clearing display buffer");
    Paint_Clear(display_white_color());

    // Detect file type by extension
    const char *ext = strrchr(filename, '.');
    bool is_png = (ext != NULL && strcasecmp(ext, ".png") == 0);
    // Check for .epdgz extension
    bool is_epdgz = (ext != NULL && strcasecmp(ext, ".epdgz") == 0);

    if (is_epdgz) {
        ESP_LOGI(TAG, "Reading EPDGZ file into buffer");
        if (GUI_ReadEPDGZ(filename) != 0) {
            ESP_LOGE(TAG, "Failed to read EPDGZ file");
            xSemaphoreGive(display_mutex);
            return ESP_FAIL;
        }
    } else if (is_png) {
        ESP_LOGI(TAG, "Reading PNG file into buffer");
        UBYTE result = display_is_grayscale() ? GUI_ReadPng_Gray16(filename, 0, 0)
                                              : GUI_ReadPng_RGB_6Color(filename, 0, 0);
        if (result != 0) {
            ESP_LOGE(TAG, "Failed to read PNG file");
            xSemaphoreGive(display_mutex);
            return ESP_FAIL;
        }
    } else {
        ESP_LOGI(TAG, "Reading BMP file into buffer");
        UBYTE result = display_is_grayscale() ? GUI_ReadBmp_RGB_Gray16(filename, 0, 0)
                                              : GUI_ReadBmp_RGB_6Color(filename, 0, 0);
        if (result != 0) {
            ESP_LOGE(TAG, "Failed to read BMP file");
            xSemaphoreGive(display_mutex);
            return ESP_FAIL;
        }
    }

    ESP_LOGI(TAG, "Starting e-paper display update (this takes ~30 seconds)");
    ESP_LOGI(TAG, "Free heap before epaper_display: %lu bytes", esp_get_free_heap_size());

    // 4. Update E-Paper Display
    // This is a blocking call that takes ~25-30 seconds for 7-color e-paper
    // It handles: Power On -> Send Data -> Refresh -> Power Off
    ESP_LOGI(TAG, "Calling epaper_display...");
    epaper_display(epd_image_buffer);
    ESP_LOGI(TAG, "epaper_display returned successfully");

    ESP_LOGI(TAG, "E-paper display update complete");
    ESP_LOGI(TAG, "Free heap after display: %lu bytes", esp_get_free_heap_size());

    strncpy(current_image, filename, sizeof(current_image) - 1);

    create_image_link(filename);
    ESP_LOGD(TAG, "Created link to: %s", filename);

    xSemaphoreGive(display_mutex);

    ESP_LOGI(TAG, "Image displayed successfully");
    return ESP_OK;
}

esp_err_t display_manager_show_rgb_buffer(const uint8_t *rgb_buffer, int width, int height)
{
    if (!rgb_buffer || width <= 0 || height <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(display_mutex, pdMS_TO_TICKS(DISPLAY_LOCK_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire display mutex");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Displaying RGB buffer: %dx%d", width, height);
    ESP_LOGI(TAG, "Free heap before display: %lu bytes", esp_get_free_heap_size());

    ESP_LOGI(TAG, "Clearing display buffer");
    Paint_Clear(display_white_color());

    ESP_LOGI(TAG, "Painting RGB buffer to display");
    UBYTE result = display_is_grayscale()
                       ? GUI_DisplayRGBBuffer_Gray16(rgb_buffer, width, height, 0, 0)
                       : GUI_DisplayRGBBuffer_6Color(rgb_buffer, width, height, 0, 0);
    if (result != 0) {
        ESP_LOGE(TAG, "Failed to paint RGB buffer");
        xSemaphoreGive(display_mutex);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Starting e-paper display update (this takes ~30 seconds)");
    ESP_LOGI(TAG, "Free heap before epaper_display: %lu bytes", esp_get_free_heap_size());

    ESP_LOGI(TAG, "Calling epaper_display...");
    epaper_display(epd_image_buffer);
    ESP_LOGI(TAG, "epaper_display returned successfully");

    ESP_LOGI(TAG, "E-paper display update complete");
    ESP_LOGI(TAG, "Free heap after display: %lu bytes", esp_get_free_heap_size());

    // Clear current_image since we displayed from buffer, not file
    current_image[0] = '\0';

    xSemaphoreGive(display_mutex);

    ESP_LOGI(TAG, "RGB buffer displayed successfully");
    return ESP_OK;
}

esp_err_t display_manager_begin_rgb_stream(void)
{
    if (xSemaphoreTake(display_mutex, pdMS_TO_TICKS(DISPLAY_LOCK_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire display mutex");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Beginning streamed RGB display");
    Paint_Clear(display_white_color());
    return ESP_OK;
}

esp_err_t display_manager_push_rgb_row(int y, const uint8_t *rgb_row, int width)
{
    if (!rgb_row) {
        return ESP_ERR_INVALID_ARG;
    }
    if (y >= Paint.Height) {
        return ESP_OK;
    }

    GUI_RGBMapFn map_rgb = display_is_grayscale() ? GUI_RGBToGray16 : GUI_RGBToSpectra6;
    for (int x = 0; x < width && x < Paint.Width; x++) {
        const uint8_t *p = &rgb_row[x * 3];
        Paint_SetPixel(x, y, map_rgb(p[0], p[1], p[2]));
    }
    return ESP_OK;
}

// zlib allocators backed by PSRAM: deflate wants ~260 KB of state, which
// should not come out of internal RAM
static voidpf zalloc_psram(voidpf opaque, uInt items, uInt size)
{
    (void) opaque;
    return heap_caps_malloc((size_t) items * size, MALLOC_CAP_SPIRAM);
}

static void zfree_psram(voidpf opaque, voidpf address)
{
    (void) opaque;
    heap_caps_free(address);
}

// Gzip-deflate the current frame to path, producing the same .epdgz format
// GUI_ReadEPDGZ renders. The reader replays the payload through
// Paint_SetPixel in logical coordinates, so pixels are read back through
// Paint_GetPixel (undoing the configured rotation/mirror) and streamed to
// the deflater one logical row at a time.
//
// Like every .epdgz in this ecosystem (converter output, splash), the
// payload is logical-orientation rows replayed under the rotation active at
// display time; rotation is restricted to 0/180 (see apply_config_from_json)
// so the dimensionless payload's row stride never changes.
static esp_err_t display_save_frame_epdgz(const char *path)
{
    const int width = Paint.Width;
    const int height = Paint.Height;
    const size_t row_bytes = ((size_t) width + 1) / 2;
    const size_t chunk = 4096;

    FILE *fp = fopen(path, "wb");
    if (!fp) {
        ESP_LOGE(TAG, "Failed to open %s for writing", path);
        return ESP_FAIL;
    }

    uint8_t *row = (uint8_t *) heap_caps_malloc(row_bytes, MALLOC_CAP_SPIRAM);
    uint8_t *out = (uint8_t *) heap_caps_malloc(chunk, MALLOC_CAP_SPIRAM);

    z_stream strm = {0};
    strm.zalloc = zalloc_psram;
    strm.zfree = zfree_psram;
    // windowBits 15+16 selects the gzip wrapper the reader expects
    bool zready = row && out &&
                  deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8,
                               Z_DEFAULT_STRATEGY) == Z_OK;

    esp_err_t err = zready ? ESP_OK : ESP_ERR_NO_MEM;

    for (int y = 0; y < height && err == ESP_OK; y++) {
        for (int x = 0; x < width; x += 2) {
            UBYTE p1 = Paint_GetPixel(x, y);
            UBYTE p2 = (x + 1 < width) ? Paint_GetPixel(x + 1, y) : 0;
            row[x / 2] = (UBYTE) ((p1 << 4) | p2);
        }

        strm.next_in = row;
        strm.avail_in = row_bytes;
        int flush = (y == height - 1) ? Z_FINISH : Z_NO_FLUSH;
        do {
            strm.next_out = out;
            strm.avail_out = chunk;
            if (deflate(&strm, flush) == Z_STREAM_ERROR) {
                err = ESP_FAIL;
                break;
            }
            size_t have = chunk - strm.avail_out;
            if (have > 0 && fwrite(out, 1, have, fp) != have) {
                ESP_LOGE(TAG, "Failed to write frame snapshot");
                err = ESP_FAIL;
                break;
            }
        } while (strm.avail_out == 0);

        // Yield periodically so the IDLE task can feed the watchdog
        if ((y & 63) == 0) {
            vTaskDelay(1);
        }
    }

    if (zready) {
        deflateEnd(&strm);
    }
    if (row) {
        heap_caps_free(row);
    }
    if (out) {
        heap_caps_free(out);
    }
    // Buffered writes can surface a full-disk error only at close
    if (fclose(fp) != 0 && err == ESP_OK) {
        ESP_LOGE(TAG, "Failed to finalize frame snapshot");
        err = ESP_FAIL;
    }

    if (err != ESP_OK) {
        unlink(path);
    } else {
        ESP_LOGI(TAG, "Saved frame snapshot: %s", path);
    }
    return err;
}

esp_err_t display_manager_push_rgb_column(int x, const uint8_t *rgb_col, int height)
{
    if (!rgb_col) {
        return ESP_ERR_INVALID_ARG;
    }
    if (x >= Paint.Width) {
        return ESP_OK;
    }

    GUI_RGBMapFn map_rgb = display_is_grayscale() ? GUI_RGBToGray16 : GUI_RGBToSpectra6;
    for (int y = 0; y < height && y < Paint.Height; y++) {
        const uint8_t *p = &rgb_col[y * 3];
        Paint_SetPixel(x, y, map_rgb(p[0], p[1], p[2]));
    }
    return ESP_OK;
}

esp_err_t display_manager_end_rgb_stream(bool show, const display_publish_t *pub)
{
    esp_err_t result = ESP_OK;

    if (show) {
        ESP_LOGI(TAG, "Starting e-paper display update (this takes ~30 seconds)");
        epaper_display(epd_image_buffer);
        ESP_LOGI(TAG, "E-paper display update complete");

        const char *record = pub ? pub->display_name : NULL;

        if (pub && pub->save_path && display_save_frame_epdgz(pub->save_path) != ESP_OK) {
            // The album entry does not exist -- publish the fallback name
            // (or nothing) instead, atomically under the display mutex, so
            // the link never points at a missing album entry
            ESP_LOGE(TAG, "Failed to save frame snapshot to %s", pub->save_path);
            record = pub->fallback_name;
            result = ESP_ERR_NOT_FINISHED;
        }

        if (record) {
            // Recorded while the mutex is still held so the reported state
            // cannot race a queued display
            strncpy(current_image, record, sizeof(current_image) - 1);
            create_image_link(record);
        } else {
            // Displayed from an anonymous buffer (or no usable fallback):
            // remove the stale link rather than reporting the previous image
            current_image[0] = '\0';
            unlink(CURRENT_IMAGE_LINK);
        }
    }

    xSemaphoreGive(display_mutex);
    return result;
}

esp_err_t display_manager_clear(void)
{
    if (xSemaphoreTake(display_mutex, pdMS_TO_TICKS(DISPLAY_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_FAIL;
    }

    epaper_clear(epd_image_buffer, EPD_7IN3E_WHITE);
    epaper_display(epd_image_buffer);

    // Remove the current image link so API returns 404
    unlink(CURRENT_IMAGE_LINK);
    current_image[0] = '\0';
    save_last_displayed_image("");

    xSemaphoreGive(display_mutex);
    return ESP_OK;
}

esp_err_t display_manager_show_calibration(void)
{
    if (xSemaphoreTake(display_mutex, pdMS_TO_TICKS(DISPLAY_LOCK_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire display mutex for calibration");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Displaying calibration pattern");

    // Re-initialize paint with current orientation
    display_manager_initialize_paint();

    // Draw the calibration pattern directly to the buffer. Grayscale (GC16)
    // panels get a 16-level gray step wedge instead of the 6-color swatches.
    if (display_is_grayscale()) {
        Paint_DrawGrayscaleCalibrationPattern();
    } else {
        Paint_DrawCalibrationPattern();
    }

    // Display the buffer
    epaper_display(epd_image_buffer);

    xSemaphoreGive(display_mutex);

    ESP_LOGI(TAG, "Calibration pattern displayed successfully");
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
                if (ext && (strcasecmp(ext, ".bmp") == 0 || strcasecmp(ext, ".png") == 0 ||
                            strcasecmp(ext, ".epdgz") == 0)) {
                    char fullpath[512];
                    snprintf(fullpath, sizeof(fullpath), "%s/%s", album_path, entry->d_name);
                    ESP_LOGD(TAG, "  Found image [%ld]: %s", (long) current_idx, fullpath);

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

    ESP_LOGI(
        TAG,
        "Sequential rotation finished traversal. current_idx=%ld, target_idx=%ld, found_target=%d",
        (long) current_idx, (long) target_idx, found_target);

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
                if (ext && (strcasecmp(ext, ".bmp") == 0 || strcasecmp(ext, ".png") == 0 ||
                            strcasecmp(ext, ".epdgz") == 0)) {
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
    if (!image_list) {
        ESP_LOGE(TAG, "Failed to allocate image list");
        return;
    }
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
                if (ext && (strcasecmp(ext, ".bmp") == 0 || strcasecmp(ext, ".png") == 0 ||
                            strcasecmp(ext, ".epdgz") == 0)) {
                    char *fullpath = malloc(512);
                    if (!fullpath) {
                        ESP_LOGE(TAG, "Failed to allocate path buffer");
                        continue;
                    }
                    snprintf(fullpath, 512, "%s/%s", album_path, entry->d_name);
                    image_list[idx] = fullpath;
                    idx++;
                }
            }
        }
        closedir(dir);
    }

    // Update total_image_count to actual number of images found
    total_image_count = idx;

    if (total_image_count == 0) {
        ESP_LOGW(TAG, "No displayable images found in enabled albums");
        free(image_list);
        return;
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

void display_manager_rotate_from_storage(void)
{
    if (!config_manager_get_auto_rotate()) {
        ESP_LOGI(TAG, "Manual rotation triggered (auto-rotate is disabled)");
    } else {
        ESP_LOGI(TAG, "Rotating from storage");
    }

    if (!storage_has_persistent_storage()) {
        ESP_LOGI(TAG, "Storage not mounted - skipping rotation");
        return;
    }

    // Get enabled albums
    char **enabled_albums = NULL;
    int album_count = 0;
    if (album_manager_get_enabled_albums(&enabled_albums, &album_count) != ESP_OK ||
        album_count == 0) {
        ESP_LOGW(TAG, "No enabled albums for auto-rotate");
        return;
    }

    ESP_LOGD(TAG, "Collecting images from %d enabled album(s)", album_count);
    for (int i = 0; i < album_count; i++) {
        ESP_LOGD(TAG, "  Enabled album[%d]: %s", i, enabled_albums[i]);
    }

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
    ESP_LOGI(TAG, "Rotation complete");
}
