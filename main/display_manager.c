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
#include "png.h"
#include "storage.h"
#include "utils.h"
#include "zlib.h"

static const char *TAG = "display_manager";
#define NVS_LAST_IMAGE_KEY "last_image"

// Display operations (streamed processing plus the panel refresh) can
// legitimately hold the display mutex for a minute or more; waiters queue
// for a matching window instead of failing spuriously.
#define DISPLAY_LOCK_TIMEOUT_MS (120 * 1000)

// Boards without PSRAM (the ESP32-C6 OpenPaper 7) cannot hold a frame buffer:
// 800x480 at 4bpp is 192 KB, which leaves too little internal RAM for WiFi,
// TLS and the PNG decoder next to it. They stream every frame, row by row,
// straight into the panel controller's own frame memory instead.
#if CONFIG_SPIRAM
#define DISPLAY_STREAM_TO_PANEL 0
#else
#define DISPLAY_STREAM_TO_PANEL 1
#endif

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

#if !DISPLAY_STREAM_TO_PANEL
static uint8_t *epd_image_buffer = NULL;
static uint32_t image_buffer_size;
#endif

// Bytes per packed frame row, two pixels per byte
static size_t frame_row_bytes(void)
{
    return ((size_t) BOARD_HAL_DISPLAY_WIDTH + 1) / 2;
}

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

#if DISPLAY_STREAM_TO_PANEL
    ESP_LOGI(TAG, "Display manager initialized (streaming to panel, no frame buffer)");
    return ESP_OK;
#else
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
#endif
}

void display_manager_initialize_paint(void)
{
#if !DISPLAY_STREAM_TO_PANEL
    Paint_NewImage(epd_image_buffer, BOARD_HAL_DISPLAY_WIDTH, BOARD_HAL_DISPLAY_HEIGHT,
                   config_manager_get_display_rotation_deg() % 360, display_white_color());
    Paint_SetScale(display_is_grayscale() ? 16 : 6);
    Paint_SelectImage(epd_image_buffer);
#endif
}

// Frame size in the logical (pre-rotation) coordinates pictures are painted in.
// Rotation is limited to 0/180 (see apply_config_from_json), so this is the
// panel's own size.
static int frame_width(void)
{
    return BOARD_HAL_DISPLAY_WIDTH;
}

static int frame_height(void)
{
    return BOARD_HAL_DISPLAY_HEIGHT;
}

// One filled rectangle of the badge, in frame coordinates, corners inclusive
typedef struct {
    int x0, y0, x1, y1;
    UWORD color;
} badge_rect_t;

#define BADGE_MAX_RECTS 7

typedef struct {
    badge_rect_t rects[BADGE_MAX_RECTS];
    int count;
} badge_t;

// Add a rectangle given in viewer space. When the picture was rotated into the
// native frame, viewer (x, y) lands at frame (width - 1 - y, x), the inverse of
// the mapping image_processor applies to the picture.
static void badge_add(badge_t *badge, bool rotated, int x0, int y0, int x1, int y1, UWORD color)
{
    if (badge->count >= BADGE_MAX_RECTS) {
        return;
    }
    badge_rect_t *r = &badge->rects[badge->count++];
    if (rotated) {
        *r = (badge_rect_t){frame_width() - 1 - y1, x0, frame_width() - 1 - y0, x1, color};
    } else {
        *r = (badge_rect_t){x0, y0, x1, y1, color};
    }
}

// Low battery badge: a depleted battery icon in the top-right corner, drawn
// over the picture right before the panel refresh so it rides on top of
// whatever picture is shown. It is drawn from the frame's own reading only,
// after the picture is complete and after any album snapshot has been saved,
// so a saved frame never carries it. Nothing is drawn while the frame is on
// USB or charging, when no battery is connected, or when the reading is
// unknown. Leaves badge->count at 0 when there is nothing to draw.
static void low_battery_badge(badge_t *badge)
{
    badge->count = 0;
#ifdef LOW_BATTERY_BADGE_DEMO_PERCENT
    // Visual check on a frame on USB: build with
    //   EXTRA_CPPFLAGS=-DLOW_BATTERY_BADGE_DEMO_PERCENT=12
    // to draw the badge on every picture.
    int percent = LOW_BATTERY_BADGE_DEMO_PERCENT;
#else
    if (!config_manager_get_low_battery_indicator()) {
        return;
    }
    if (!board_hal_is_battery_connected() || board_hal_is_usb_connected() ||
        board_hal_is_charging()) {
        return;
    }
    int percent = board_hal_get_battery_percent();
    if (percent < 0 || percent > 100 || percent > config_manager_get_low_battery_threshold()) {
        return;
    }
#endif
    ESP_LOGI(TAG, "Battery at %d%%, drawing low battery badge", percent);

    // The badge is laid out in the viewer's space: the picture as hung. That
    // is the native frame unless the configured orientation differs from the
    // panel's native layout, in which case image_processor rotated the picture
    // into the frame and the badge follows the same mapping, so it stays in
    // the viewer's top-right corner with the battery lying flat.
    const bool rotated =
        (frame_width() > frame_height()) !=
        (config_manager_get_display_orientation() == DISPLAY_ORIENTATION_LANDSCAPE);
    const int view_w = rotated ? frame_height() : frame_width();
    const int view_h = rotated ? frame_width() : frame_height();

    // Everything scales with the panel: one unit per 480 rows of the shorter
    // side, so the badge keeps its proportion from 800x480 to 1600x1200.
    const int u = (view_w < view_h ? view_w : view_h) >= 960 ? 2 : 1;
    const UWORD white = display_white_color();
    const UWORD black = display_is_grayscale() ? 0x0 : EPD_7IN3E_BLACK;
    const UWORD alert = display_is_grayscale() ? 0x0 : EPD_7IN3E_RED;

    const int pad = 10 * u;
    const int margin = 16 * u;
    const int icon_w = 48 * u;
    const int icon_h = 24 * u;
    const int nub_w = 5 * u;
    const int nub_h = 10 * u;
    const int stroke = 2 * u;
    const int box_w = pad + icon_w + nub_w + pad;
    const int box_h = icon_h + 2 * pad;
    if (box_w + 2 * margin > view_w || box_h + 2 * margin > view_h) {
        return;
    }
    const int x0 = view_w - margin - box_w;
    const int y0 = margin;

    // White card behind the icon so it reads on any picture
    badge_add(badge, rotated, x0, y0, x0 + box_w - 1, y0 + box_h - 1, white);

    // Depleted battery: outline, terminal nub and a thin sliver of charge
    const int ix = x0 + pad;
    const int iy = y0 + pad;
    const int ix1 = ix + icon_w - 1;
    const int iy1 = iy + icon_h - 1;
    badge_add(badge, rotated, ix, iy, ix1, iy + stroke - 1, black);
    badge_add(badge, rotated, ix, iy1 - stroke + 1, ix1, iy1, black);
    badge_add(badge, rotated, ix, iy, ix + stroke - 1, iy1, black);
    badge_add(badge, rotated, ix1 - stroke + 1, iy, ix1, iy1, black);
    badge_add(badge, rotated, ix1 + 1, iy + (icon_h - nub_h) / 2, ix1 + nub_w,
              iy + (icon_h + nub_h) / 2 - 1, black);
    const int inset = 5 * u;
    badge_add(badge, rotated, ix + inset, iy + inset, ix + inset + 6 * u - 1, iy1 - inset, alert);
}

#if !DISPLAY_STREAM_TO_PANEL

// Draw the low battery badge, if any, into the frame buffer
static void paint_low_battery_badge(void)
{
    badge_t badge;
    low_battery_badge(&badge);
    for (int i = 0; i < badge.count; i++) {
        const badge_rect_t *r = &badge.rects[i];
        Paint_DrawRectangle(r->x0, r->y0, r->x1, r->y1, r->color, DOT_PIXEL_1X1, DRAW_FILL_FULL);
    }
}

esp_err_t display_manager_show_image(const char *filename)
{
    if (!filename || strlen(filename) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(display_mutex, pdMS_TO_TICKS(DISPLAY_LOCK_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire display mutex");
        return ESP_ERR_TIMEOUT;
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
    paint_low_battery_badge();

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
        return ESP_ERR_TIMEOUT;
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

    paint_low_battery_badge();

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

esp_err_t display_manager_begin_rgb_stream(const display_publish_t *pub)
{
    // The snapshot (pub->save_path) is taken from the finished frame buffer
    // in display_manager_end_rgb_stream
    (void) pub;

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
        const char *record = pub ? pub->display_name : NULL;

        // The snapshot is taken before the badge goes on, so the album keeps
        // the bare picture
        if (pub && pub->save_path && display_save_frame_epdgz(pub->save_path) != ESP_OK) {
            // The album entry does not exist -- publish the fallback name
            // (or nothing) instead, atomically under the display mutex, so
            // the link never points at a missing album entry
            ESP_LOGE(TAG, "Failed to save frame snapshot to %s", pub->save_path);
            record = pub->fallback_name;
            result = ESP_ERR_NOT_FINISHED;
        }

        paint_low_battery_badge();

        ESP_LOGI(TAG, "Starting e-paper display update (this takes ~30 seconds)");
        epaper_display(epd_image_buffer);
        ESP_LOGI(TAG, "E-paper display update complete");

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

esp_err_t display_manager_show_rows(display_row_fill_fn fill, void *ctx)
{
    if (xSemaphoreTake(display_mutex, pdMS_TO_TICKS(DISPLAY_LOCK_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire display mutex");
        return ESP_ERR_TIMEOUT;
    }

    // Native rows straight into the buffer: no Paint rotation, no badge
    const size_t row_bytes = frame_row_bytes();
    esp_err_t err = ESP_OK;
    for (int y = 0; y < BOARD_HAL_DISPLAY_HEIGHT && err == ESP_OK; y++) {
        err = fill(ctx, y, epd_image_buffer + (size_t) y * row_bytes);
    }
    if (err == ESP_OK) {
        epaper_display(epd_image_buffer);
    }

    xSemaphoreGive(display_mutex);
    return err;
}

#else  // DISPLAY_STREAM_TO_PANEL

// A frame is produced top row first, in frame (pre-rotation) coordinates, one
// packed row at a time. Each row is
//   1. deflated into the album snapshot, when one was asked for -- so, as on
//      the frame-buffer path, the saved picture never carries the badge,
//   2. overlaid with the low battery badge, and
//   3. sent on to the panel controller.
// A 180 degree display rotation is done by the controller's scan direction.
// Nothing reaches the screen before the refresh at the end, so a frame given
// up halfway leaves the previous picture in place.

// deflate state at the zlib defaults is ~260 KB; at windowBits 12 / memLevel 5
// it is ~38 KB, and any gzip reader still inflates the result
#define SNAPSHOT_WINDOW_BITS 12
#define SNAPSHOT_MEM_LEVEL 5
#define SNAPSHOT_OUT_CHUNK 1024

typedef struct {
    uint8_t *row;  // frame row next_y, being assembled by the producer
    int next_y;
    badge_t badge;

    const char *snap_path;  // NULL when no snapshot was asked for
    FILE *snap_fp;
    z_stream snap;
    uint8_t *snap_out;
    bool snap_ready;  // deflater initialized
    bool snap_failed;
} panel_stream_t;

static panel_stream_t ps;

static void snapshot_drain(int flush)
{
    do {
        ps.snap.next_out = ps.snap_out;
        ps.snap.avail_out = SNAPSHOT_OUT_CHUNK;
        if (deflate(&ps.snap, flush) == Z_STREAM_ERROR) {
            ps.snap_failed = true;
            return;
        }
        size_t have = SNAPSHOT_OUT_CHUNK - ps.snap.avail_out;
        if (have > 0 && fwrite(ps.snap_out, 1, have, ps.snap_fp) != have) {
            ESP_LOGE(TAG, "Failed to write frame snapshot");
            ps.snap_failed = true;
            return;
        }
    } while (ps.snap.avail_out == 0);
}

static void snapshot_open(const char *path)
{
    ps.snap_path = path;
    ps.snap_fp = fopen(path, "wb");
    ps.snap_out = malloc(SNAPSHOT_OUT_CHUNK);
    // windowBits + 16 selects the gzip wrapper GUI_ReadEPDGZ expects
    ps.snap_ready =
        ps.snap_fp && ps.snap_out &&
        deflateInit2(&ps.snap, Z_DEFAULT_COMPRESSION, Z_DEFLATED, SNAPSHOT_WINDOW_BITS + 16,
                     SNAPSHOT_MEM_LEVEL, Z_DEFAULT_STRATEGY) == Z_OK;
    if (!ps.snap_ready) {
        ESP_LOGE(TAG, "Failed to start frame snapshot %s", path);
        ps.snap_failed = true;
    }
}

static void snapshot_write_row(const uint8_t *row)
{
    if (!ps.snap_ready || ps.snap_failed) {
        return;
    }
    ps.snap.next_in = (Bytef *) row;
    ps.snap.avail_in = frame_row_bytes();
    snapshot_drain(Z_NO_FLUSH);
}

// Finish the snapshot; keep it only if the frame was shown and every row made
// it into the file
static esp_err_t snapshot_close(bool keep)
{
    if (ps.snap_ready) {
        if (keep && !ps.snap_failed) {
            ps.snap.next_in = NULL;
            ps.snap.avail_in = 0;
            snapshot_drain(Z_FINISH);
        }
        deflateEnd(&ps.snap);
    }
    free(ps.snap_out);
    // Buffered writes can surface a full-disk error only at close
    if (ps.snap_fp && fclose(ps.snap_fp) != 0) {
        ps.snap_failed = true;
    }

    esp_err_t err = (keep && !ps.snap_failed) ? ESP_OK : ESP_FAIL;
    if (err != ESP_OK) {
        unlink(ps.snap_path);
    } else {
        ESP_LOGI(TAG, "Saved frame snapshot: %s", ps.snap_path);
    }
    return err;
}

static void row_set_pixel(uint8_t *row, int x, uint8_t color)
{
    uint8_t *p = &row[x / 2];
    if (x % 2 == 0) {
        *p = (uint8_t) ((*p & 0x0F) | (color << 4));
    } else {
        *p = (uint8_t) ((*p & 0xF0) | (color & 0x0F));
    }
}

static void stream_row_fill_white(void)
{
    memset(ps.row, display_white_color() * 0x11, frame_row_bytes());
}

// Start a streamed frame. rotate applies the configured display rotation
// (pictures), badge the low battery badge; a snapshot_path saves the frame,
// without the badge, as .epdgz once it is shown.
static esp_err_t stream_begin(const char *snapshot_path, bool rotate, bool badge)
{
    ps = (panel_stream_t){0};
    ps.row = malloc(frame_row_bytes());
    if (!ps.row) {
        ESP_LOGE(TAG, "Failed to allocate row buffer");
        return ESP_ERR_NO_MEM;
    }
    if (snapshot_path) {
        snapshot_open(snapshot_path);
    }
    if (badge) {
        low_battery_badge(&ps.badge);
    }
    epaper_stream_begin(rotate && config_manager_get_display_rotation_deg() == 180);
    return ESP_OK;
}

// Send ps.row as frame row ps.next_y
static void stream_send_row(void)
{
    if (ps.snap_path) {
        snapshot_write_row(ps.row);
    }

    const int y = ps.next_y;
    for (int i = 0; i < ps.badge.count; i++) {
        const badge_rect_t *r = &ps.badge.rects[i];
        if (y >= r->y0 && y <= r->y1) {
            for (int x = r->x0; x <= r->x1; x++) {
                row_set_pixel(ps.row, x, r->color);
            }
        }
    }

    epaper_stream_write(ps.row, frame_row_bytes());
    ps.next_y++;

    // Yield periodically so the IDLE task can feed the watchdog
    if ((ps.next_y & 63) == 0) {
        vTaskDelay(1);
    }
}

// Finish the frame: refresh pads any rows not produced with white, then shows
// it; otherwise the frame is abandoned. Returns the snapshot's outcome (ESP_OK
// when none was asked for).
static esp_err_t stream_end(bool refresh)
{
    if (refresh) {
        while (ps.next_y < frame_height()) {
            stream_row_fill_white();
            stream_send_row();
        }
    }

    esp_err_t snap_err = ESP_OK;
    if (ps.snap_path) {
        snap_err = snapshot_close(refresh);
    }

    if (refresh) {
        ESP_LOGI(TAG, "Starting e-paper display update (this takes ~30 seconds)");
    }
    epaper_stream_end(refresh);

    free(ps.row);
    ps.row = NULL;
    return snap_err;
}

// Map an RGB row onto the frame row, clipped to the frame width
static void stream_row_from_rgb(const uint8_t *rgb, int width)
{
    GUI_RGBMapFn map_rgb = display_is_grayscale() ? GUI_RGBToGray16 : GUI_RGBToSpectra6;
    stream_row_fill_white();
    for (int x = 0; x < width && x < frame_width(); x++) {
        const uint8_t *p = &rgb[x * 3];
        row_set_pixel(ps.row, x, map_rgb(p[0], p[1], p[2]));
    }
}

static int stream_epdgz_row(void *ctx, int y, const uint8_t *row)
{
    (void) ctx;
    memcpy(ps.row, row, frame_row_bytes());
    stream_send_row();
    return 0;
}

// Stream a PNG file's rows, clipped to the frame; rows past its bottom edge are
// left to stream_end's white padding
static int stream_png_file(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "Cannot open PNG file: %s", path);
        return 1;
    }

    uint8_t sig[8];
    if (fread(sig, 1, 8, fp) != 8 || png_sig_cmp(sig, 0, 8) != 0) {
        ESP_LOGE(TAG, "Not a valid PNG file");
        fclose(fp);
        return 1;
    }

    png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop info_ptr = png_ptr ? png_create_info_struct(png_ptr) : NULL;
    png_bytep volatile row = NULL;
    int result = 1;
    if (!png_ptr || !info_ptr) {
        goto done;
    }

    if (setjmp(png_jmpbuf(png_ptr))) {
        ESP_LOGE(TAG, "PNG decode error");
        goto done;
    }

    png_init_io(png_ptr, fp);
    png_set_sig_bytes(png_ptr, 8);
    png_read_info(png_ptr, info_ptr);

    int width = png_get_image_width(png_ptr, info_ptr);
    int height = png_get_image_height(png_ptr, info_ptr);
    if (png_get_interlace_type(png_ptr, info_ptr) != PNG_INTERLACE_NONE) {
        ESP_LOGE(TAG, "Interlaced PNG cannot be streamed");
        goto done;
    }

    // Any PNG flavor to 8-bit RGB
    png_set_expand(png_ptr);
    png_set_strip_16(png_ptr);
    png_set_strip_alpha(png_ptr);
    png_set_packing(png_ptr);
    png_set_gray_to_rgb(png_ptr);
    png_read_update_info(png_ptr, info_ptr);
    if (png_get_channels(png_ptr, info_ptr) != 3) {
        ESP_LOGE(TAG, "Unexpected PNG channel count");
        goto done;
    }

    row = (png_bytep) malloc(png_get_rowbytes(png_ptr, info_ptr));
    if (!row) {
        ESP_LOGE(TAG, "Failed to allocate PNG row buffer");
        goto done;
    }

    for (int y = 0; y < height && y < frame_height(); y++) {
        png_read_row(png_ptr, row, NULL);
        stream_row_from_rgb(row, width);
        stream_send_row();
    }
    result = 0;

done:
    free((void *) row);
    if (png_ptr) {
        png_destroy_read_struct(&png_ptr, info_ptr ? &info_ptr : NULL, NULL);
    }
    fclose(fp);
    return result;
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) |
           ((uint32_t) p[3] << 24);
}

// Largest BMP width or height accepted
#define BMP_MAX_DIM 16384

// Stream a 24-bit BMP file's rows, clipped to the frame. Rows are fetched by
// seeking, since BMPs usually store them bottom to top.
static int stream_bmp_file(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "Cannot open BMP file: %s", path);
        return 1;
    }

    uint8_t hdr[54];
    if (fread(hdr, 1, sizeof(hdr), fp) != sizeof(hdr) || hdr[0] != 'B' || hdr[1] != 'M') {
        ESP_LOGE(TAG, "Not a valid BMP file");
        fclose(fp);
        return 1;
    }
    uint32_t offset = le32(&hdr[10]);
    int32_t width = (int32_t) le32(&hdr[18]);
    int32_t height = (int32_t) le32(&hdr[22]);
    uint16_t bpp = (uint16_t) (hdr[28] | (hdr[29] << 8));
    bool top_down = height < 0;
    if (top_down) {
        height = -height;
    }
    // The size cap keeps the row size and file offsets below from overflowing
    // a 32-bit size_t and long
    if (bpp != 24 || width <= 0 || height <= 0 || width > BMP_MAX_DIM || height > BMP_MAX_DIM) {
        ESP_LOGE(TAG, "Unsupported BMP (%ldx%ld, %u bpp)", (long) width, (long) height, bpp);
        fclose(fp);
        return 1;
    }

    size_t row_padded = ((size_t) width * 3 + 3) & ~(size_t) 3;
    uint8_t *row = malloc(row_padded);
    if (!row) {
        ESP_LOGE(TAG, "Failed to allocate BMP row buffer");
        fclose(fp);
        return 1;
    }

    int result = 0;
    for (int y = 0; y < height && y < frame_height(); y++) {
        long file_row = top_down ? y : (height - 1 - y);
        if (fseek(fp, (long) offset + file_row * (long) row_padded, SEEK_SET) != 0 ||
            fread(row, 1, row_padded, fp) != row_padded) {
            ESP_LOGE(TAG, "BMP read failed at row %d", y);
            result = 1;
            break;
        }
        // BMP is BGR; only the columns inside the frame are used
        for (int32_t x = 0; x < width && x < frame_width(); x++) {
            uint8_t t = row[x * 3];
            row[x * 3] = row[x * 3 + 2];
            row[x * 3 + 2] = t;
        }
        stream_row_from_rgb(row, width);
        stream_send_row();
    }

    free(row);
    fclose(fp);
    return result;
}

// Record the image now on the panel for /api/current_image; NULL clears it
static void publish_current_image(const char *record)
{
    if (record) {
        strncpy(current_image, record, sizeof(current_image) - 1);
        create_image_link(record);
    } else {
        current_image[0] = '\0';
        unlink(CURRENT_IMAGE_LINK);
    }
}

esp_err_t display_manager_show_image(const char *filename)
{
    if (!filename || strlen(filename) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(display_mutex, pdMS_TO_TICKS(DISPLAY_LOCK_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire display mutex");
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "Displaying image: %s", filename);
    ESP_LOGI(TAG, "Free heap before display: %lu bytes", esp_get_free_heap_size());

    esp_err_t err = stream_begin(NULL, true, true);
    if (err != ESP_OK) {
        xSemaphoreGive(display_mutex);
        return err;
    }

    const char *ext = strrchr(filename, '.');
    int rc;
    if (ext && strcasecmp(ext, ".epdgz") == 0) {
        rc = GUI_ReadEPDGZRows(filename, frame_row_bytes(), frame_height(), stream_epdgz_row, NULL);
    } else if (ext && strcasecmp(ext, ".png") == 0) {
        rc = stream_png_file(filename);
    } else {
        rc = stream_bmp_file(filename);
    }

    stream_end(rc == 0);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to read %s", filename);
        xSemaphoreGive(display_mutex);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Free heap after display: %lu bytes", esp_get_free_heap_size());
    publish_current_image(filename);

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
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = stream_begin(NULL, true, true);
    if (err == ESP_OK) {
        for (int y = 0; y < height && y < frame_height(); y++) {
            stream_row_from_rgb(rgb_buffer + (size_t) y * width * 3, width);
            stream_send_row();
        }
        stream_end(true);
        // Displayed from a buffer, not a file
        current_image[0] = '\0';
    }

    xSemaphoreGive(display_mutex);
    return err;
}

esp_err_t display_manager_begin_rgb_stream(const display_publish_t *pub)
{
    if (xSemaphoreTake(display_mutex, pdMS_TO_TICKS(DISPLAY_LOCK_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire display mutex");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Beginning streamed RGB display (direct to panel)");
    esp_err_t err = stream_begin(pub ? pub->save_path : NULL, true, true);
    if (err != ESP_OK) {
        xSemaphoreGive(display_mutex);
    }
    return err;
}

esp_err_t display_manager_push_rgb_row(int y, const uint8_t *rgb_row, int width)
{
    if (!rgb_row) {
        return ESP_ERR_INVALID_ARG;
    }
    if (y >= frame_height()) {
        return ESP_OK;
    }
    if (y < ps.next_y) {
        // The panel takes rows strictly top to bottom
        ESP_LOGE(TAG, "Row %d arrived after row %d", y, ps.next_y - 1);
        return ESP_ERR_INVALID_STATE;
    }

    while (ps.next_y < y) {
        stream_row_fill_white();
        stream_send_row();
    }
    stream_row_from_rgb(rgb_row, width);
    stream_send_row();
    return ESP_OK;
}

esp_err_t display_manager_push_rgb_column(int x, const uint8_t *rgb_col, int height)
{
    // Columns cannot be streamed to the panel, which takes whole rows top to
    // bottom. Only pictures that need a 90 degree turn into the panel's
    // layout come this way, and those need processing this board can't do.
    ESP_LOGE(TAG, "Rotated (column) streaming is not supported without PSRAM");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t display_manager_end_rgb_stream(bool show, const display_publish_t *pub)
{
    esp_err_t result = ESP_OK;
    esp_err_t snap_err = stream_end(show);

    if (show) {
        const char *record = pub ? pub->display_name : NULL;
        if (pub && pub->save_path && snap_err != ESP_OK) {
            // The album entry does not exist -- publish the fallback name
            // (or nothing) instead, atomically under the display mutex, so
            // the link never points at a missing album entry
            ESP_LOGE(TAG, "Failed to save frame snapshot to %s", pub->save_path);
            record = pub->fallback_name;
            result = ESP_ERR_NOT_FINISHED;
        }
        ESP_LOGI(TAG, "E-paper display update complete");
        publish_current_image(record);
    }

    xSemaphoreGive(display_mutex);
    return result;
}

esp_err_t display_manager_clear(void)
{
    if (xSemaphoreTake(display_mutex, pdMS_TO_TICKS(DISPLAY_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_FAIL;
    }

    // stream_end pads every row with white
    esp_err_t err = stream_begin(NULL, false, false);
    if (err == ESP_OK) {
        stream_end(true);

        // Remove the current image link so API returns 404
        publish_current_image(NULL);
        save_last_displayed_image("");
    }

    xSemaphoreGive(display_mutex);
    return err;
}

esp_err_t display_manager_show_calibration(void)
{
    if (xSemaphoreTake(display_mutex, pdMS_TO_TICKS(DISPLAY_LOCK_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire display mutex for calibration");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Displaying calibration pattern");

    esp_err_t err = stream_begin(NULL, true, false);
    if (err == ESP_OK) {
        const int w = frame_width();
        const int h = frame_height();
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                // Grayscale (GC16) panels get a 16-level gray step wedge
                // instead of the 6-color swatches
                UBYTE c = display_is_grayscale()
                              ? Paint_GrayscaleCalibrationPatternColor(x, y, w, h)
                              : Paint_CalibrationPatternColor(x, y, w, h);
                row_set_pixel(ps.row, x, c);
            }
            stream_send_row();
        }
        stream_end(true);
        ESP_LOGI(TAG, "Calibration pattern displayed successfully");
    }

    xSemaphoreGive(display_mutex);
    return err;
}

esp_err_t display_manager_show_rows(display_row_fill_fn fill, void *ctx)
{
    if (xSemaphoreTake(display_mutex, pdMS_TO_TICKS(DISPLAY_LOCK_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire display mutex");
        return ESP_ERR_TIMEOUT;
    }

    // Native rows: no rotation, no badge
    esp_err_t err = stream_begin(NULL, false, false);
    if (err == ESP_OK) {
        for (int y = 0; y < frame_height() && err == ESP_OK; y++) {
            err = fill(ctx, y, ps.row);
            if (err == ESP_OK) {
                stream_send_row();
            }
        }
        stream_end(err == ESP_OK);
    }

    xSemaphoreGive(display_mutex);
    return err;
}
#endif  // DISPLAY_STREAM_TO_PANEL

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
                        esp_err_t shown = display_manager_show_image(fullpath);
                        if (shown == ESP_ERR_TIMEOUT) {
                            // The panel is busy with something else; another
                            // file would only wait on the same lock again.
                            ESP_LOGE(TAG, "Display busy; giving up this rotation");
                            closedir(dir);
                            return;
                        }
                        if (shown != ESP_OK) {
                            // Display failed (e.g. a transient SD-card read
                            // glitch decoding this one file) - move the
                            // target forward one and let the walk continue;
                            // the very next image found becomes the new
                            // target instead of leaving a half-rendered panel
                            // up until the next scheduled rotation.
                            ESP_LOGW(TAG, "Failed to display %s, trying the next image instead",
                                     fullpath);
                            target_idx++;
                            current_idx++;
                            continue;
                        }
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
            if (display_manager_show_image(first_image) == ESP_OK) {
                save_last_displayed_image(first_image);
                config_manager_set_last_index(0);  // Reset index to 0
            } else {
                // Nothing left to fall back to - this was already the last
                // resort. The next scheduled rotation tries fresh.
                ESP_LOGE(TAG, "Failed to display %s on wrap-around; giving up for this rotation",
                         first_image);
            }
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
    esp_err_t shown = display_manager_show_image(image_list[random_index]);
    if (shown == ESP_OK) {
        // Store the displayed image filename in NVS
        save_last_displayed_image(image_list[random_index]);
    } else if (shown == ESP_ERR_TIMEOUT) {
        // The panel is busy with something else; another file would only
        // wait on the same lock again.
        ESP_LOGE(TAG, "Display busy; giving up this rotation");
    } else {
        ESP_LOGW(TAG, "Failed to display %s, trying a different image instead",
                 image_list[random_index]);
        // Recovery from a transient read glitch (e.g. a flaky SD-card access
        // on just this one file), not a retry loop: exactly one different
        // image from the same candidate pool, and give up for this rotation if
        // that fails too; the next scheduled rotation tries fresh either way.
        int retry_index = (total_image_count > 1) ? random_index : -1;
        while (retry_index == random_index && total_image_count > 1) {
            retry_index = (int) (esp_random() % total_image_count);
        }
        if (retry_index < 0 || display_manager_show_image(image_list[retry_index]) != ESP_OK) {
            ESP_LOGE(TAG,
                     "Auto-rotate: replacement image also failed to display; giving up "
                     "for this rotation");
        } else {
            ESP_LOGI(TAG, "Auto-rotate: recovered by displaying %s instead",
                     image_list[retry_index]);
            save_last_displayed_image(image_list[retry_index]);
        }
    }

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
