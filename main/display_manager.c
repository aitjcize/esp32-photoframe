#include "display_manager.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "GUI_BMPfile.h"
#include "GUI_ColorMap.h"
#include "GUI_EPDGZfile.h"
#include "GUI_PNGfile.h"
#include "GUI_Paint.h"
#include "GUI_RawBuffer.h"
#include "album_manager.h"
#include "battery_history.h"
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
#include "history_manager.h"
#include "image_processor.h"
#include "nvs.h"
#include "processing_settings.h"
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
                   config_manager_get_display_rotation_deg() % 360, EPD_7IN3E_WHITE);
    Paint_SetScale(6);
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
    Paint_Clear(EPD_7IN3E_WHITE);

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

    // Single choke point for every successful display, regardless of source
    // (rotation, manual web-UI pick, Telegram) - keeps the "shown until a
    // full cycle completes" history consistent everywhere. Idempotent: a
    // repeated path (e.g. the fixed URL-rotation temp file) is a no-op after
    // the first call.
    history_manager_mark_shown(filename);

    // One battery reading per successfully displayed image - see
    // battery_history.c for the persisted log and reset policy.
    battery_history_record();

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
    Paint_Clear(EPD_7IN3E_WHITE);

    ESP_LOGI(TAG, "Painting RGB buffer to display");
    if (GUI_DisplayRGBBuffer_6Color(rgb_buffer, width, height, 0, 0) != 0) {
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
    if (strncmp(BOARD_HAL_DISPLAY_TYPE, "gc", 2) == 0) {
        Paint_SetScale(16);
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

// Whether the frame is currently mounted in portrait as *actually rendered*.
// Mirrors telegram_bot.c's wants_portrait_frame_now() - kept as a separate,
// tiny duplicate rather than a cross-module dependency between these two
// already-large files.
static bool wants_portrait_frame(void)
{
    int rot = config_manager_get_display_rotation_deg() % 360;
    if (rot < 0) {
        rot += 360;
    }
    return (rot == 90 || rot == 270);
}

static esp_err_t read_whole_file_dm(const char *path, uint8_t **out_data, long *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return ESP_FAIL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        fclose(f);
        return ESP_FAIL;
    }
    uint8_t *buf = heap_caps_malloc((size_t) size, MALLOC_CAP_SPIRAM);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t read_bytes = fread(buf, 1, (size_t) size, f);
    fclose(f);
    if (read_bytes != (size_t) size) {
        heap_caps_free(buf);
        return ESP_FAIL;
    }
    *out_data = buf;
    *out_size = size;
    return ESP_OK;
}

// Composes two mismatched-orientation album images into one, saved as a new
// permanent file in dest_album_path - same idea as telegram_bot.c's
// orientation pairing, applied to normal auto-rotation instead of Telegram
// receives. Both source files are left untouched (caller's choice to keep
// them as independently rotatable images too).
static esp_err_t compose_rotation_pair(const char *path_a, const char *path_b,
                                       const char *dest_album_path, char *out_path,
                                       size_t out_path_len)
{
    uint8_t *buf_a = NULL, *buf_b = NULL;
    long size_a = 0, size_b = 0;

    esp_err_t err = read_whole_file_dm(path_a, &buf_a, &size_a);
    if (err != ESP_OK) {
        return err;
    }
    err = read_whole_file_dm(path_b, &buf_b, &size_b);
    if (err != ESP_OK) {
        heap_caps_free(buf_a);
        return err;
    }

    image_format_t format_a = image_processor_detect_format(path_a);
    image_format_t format_b = image_processor_detect_format(path_b);
    dither_algorithm_t algo = processing_settings_get_dithering_algorithm();

    image_process_rgb_result_t result;
    err = image_processor_compose_pair_to_rgb(buf_a, (size_t) size_a, format_a, buf_b,
                                              (size_t) size_b, format_b, wants_portrait_frame(),
                                              algo, &result);
    heap_caps_free(buf_a);
    heap_caps_free(buf_b);
    if (err != ESP_OK) {
        return err;
    }

    time_t now = time(NULL);
    for (int suffix = 0; suffix < 100; suffix++) {
        if (suffix == 0) {
            snprintf(out_path, out_path_len, "%s/combined_%lld.png", dest_album_path,
                     (long long) now);
        } else {
            snprintf(out_path, out_path_len, "%s/combined_%lld_%d.png", dest_album_path,
                     (long long) now, suffix);
        }
        struct stat st;
        if (stat(out_path, &st) != 0) {
            break;  // path is free
        }
    }

    err = image_processor_write_rgb_to_png(result.rgb_data, result.width, result.height, out_path);
    heap_caps_free(result.rgb_data);
    return err;
}

// Peeks orientation for a single candidate; only PNG/JPG carry a meaningful
// source aspect ratio to check (BMP/EPDGZ are treated as already
// display-appropriate, same as in telegram_bot.c's pairing).
static bool image_orientation_mismatches(const char *path, bool wants_portrait)
{
    image_format_t fmt = image_processor_detect_format(path);
    if (fmt != IMAGE_FORMAT_PNG && fmt != IMAGE_FORMAT_JPG) {
        return false;
    }
    int w = 0, h = 0;
    if (image_processor_peek_file_dimensions(path, fmt, &w, &h) != ESP_OK || w <= 0 || h <= 0) {
        return false;
    }
    return (h > w) != wants_portrait;
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

    // Pick among images not yet shown this cycle (history_manager). If every
    // image in the enabled albums has already been shown, the cycle is
    // complete: clear the history and start a fresh one over the full set.
    // This also inherently avoids repeating the last-shown image whenever
    // more than one image remains unseen, so no separate retry-loop is
    // needed for that anymore.
    int *unseen = malloc((size_t) total_image_count * sizeof(int));
    if (!unseen) {
        ESP_LOGE(TAG, "Failed to allocate unseen-index list");
        for (int i = 0; i < total_image_count; i++) {
            free(image_list[i]);
        }
        free(image_list);
        return;
    }
    int unseen_count = 0;
    for (int i = 0; i < total_image_count; i++) {
        if (!history_manager_has_shown(image_list[i])) {
            unseen[unseen_count++] = i;
        }
    }
    if (unseen_count == 0) {
        ESP_LOGI(TAG, "Display history cycle complete (%d images shown) - starting a new cycle",
                 total_image_count);
        history_manager_clear();
        for (int i = 0; i < total_image_count; i++) {
            unseen[unseen_count++] = i;
        }
    }

    int random_index = unseen[esp_random() % unseen_count];
    free(unseen);

    const char *display_path = image_list[random_index];
    char composed_path[512];
    bool use_composed = false;

    // Orientation pairing (opt-in, random mode only - see
    // config_manager_get_rotation_pairing_enabled()): if the picked image
    // doesn't match the panel's orientation, look for another mismatched
    // image in the same candidate pool and combine them instead of showing
    // one letterboxed.
    if (config_manager_get_rotation_pairing_enabled()) {
        bool wants_portrait = wants_portrait_frame();
        if (image_orientation_mismatches(display_path, wants_portrait)) {
            int partner_index = -1;
            for (int i = 0; i < total_image_count; i++) {
                if (i == random_index) {
                    continue;
                }
                if (image_orientation_mismatches(image_list[i], wants_portrait)) {
                    partner_index = i;
                    break;
                }
            }

            if (partner_index >= 0) {
                // Save the combined result into the primary pick's own
                // album directory.
                char dest_album_path[512];
                strncpy(dest_album_path, display_path, sizeof(dest_album_path) - 1);
                dest_album_path[sizeof(dest_album_path) - 1] = '\0';
                char *slash = strrchr(dest_album_path, '/');
                if (slash) {
                    *slash = '\0';
                }

                esp_err_t err = compose_rotation_pair(display_path, image_list[partner_index],
                                                      dest_album_path, composed_path,
                                                      sizeof(composed_path));
                if (err == ESP_OK) {
                    ESP_LOGI(TAG, "Auto-rotate: combined %s + %s -> %s", display_path,
                             image_list[partner_index], composed_path);
                    // The sources stay in their album (still independently
                    // rotatable later) but count as shown for this cycle,
                    // same as the combined result itself will once displayed.
                    history_manager_mark_shown(display_path);
                    history_manager_mark_shown(image_list[partner_index]);
                    use_composed = true;
                } else {
                    ESP_LOGW(TAG, "Auto-rotate: failed to combine %s + %s: %s", display_path,
                             image_list[partner_index], esp_err_to_name(err));
                }
            }
        }
    }

    const char *final_path = use_composed ? composed_path : display_path;

    // Display random image
    ESP_LOGI(TAG, "Auto-rotate: Displaying random image %d/%d (unseen this cycle: %d): %s",
             random_index + 1, total_image_count, unseen_count, final_path);
    display_manager_show_image(final_path);

    // Store the displayed image filename in NVS
    save_last_displayed_image(final_path);

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
