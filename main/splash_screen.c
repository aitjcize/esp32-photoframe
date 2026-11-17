#include "splash_screen.h"

#include <stdio.h>
#include <string.h>
#include <zlib.h>

#include "board_hal.h"
#include "display_manager.h"
#include "esp_log.h"
#include "qrcode.h"
#include "splash_data/splash_meta.h"
#include "utils.h"

static const char *TAG = "splash_screen";

// Embedded EPDGZ screens (generated at build time for the target board)
extern const uint8_t splash_epdgz_start[] asm("_binary_splash_epdgz_start");
extern const uint8_t splash_epdgz_end[] asm("_binary_splash_epdgz_end");
extern const uint8_t setup_complete_epdgz_start[] asm("_binary_setup_complete_epdgz_start");
extern const uint8_t setup_complete_epdgz_end[] asm("_binary_setup_complete_epdgz_end");

// E-paper palette indices
#define EPD_BLACK 0
#define EPD_WHITE 1

// On grayscale (gc*) panels "white" is gray level 15, not the Spectra index 1;
// drawing the QR with index 1 puts its quiet zone at near-black.
static inline uint8_t splash_white_nibble(void)
{
    return strncmp(BOARD_HAL_DISPLAY_TYPE, "gc", 2) == 0 ? 0x0F : EPD_WHITE;
}

/**
 * Set a pixel in a packed 4-bit-per-pixel frame row.
 * Two pixels per byte: high nibble = even pixel, low nibble = odd pixel.
 */
static void set_row_pixel(uint8_t *row, int x, uint8_t color)
{
    if (x < 0 || x >= BOARD_HAL_DISPLAY_WIDTH)
        return;

    if (x % 2 == 0) {
        row[x / 2] = (row[x / 2] & 0x0F) | (color << 4);
    } else {
        row[x / 2] = (row[x / 2] & 0xF0) | (color & 0x0F);
    }
}

// Largest QR the generator is allowed (version 10): 4 * 10 + 17 modules a side
#define QR_MAX_VERSION 10
#define QR_MAX_MODULES (4 * QR_MAX_VERSION + 17)

// A generated QR code and where it goes on the screen
typedef struct {
    int pos_x;
    int pos_y;
    int target_size;
    int size;  // modules per side, 0 until generated
    uint8_t modules[QR_MAX_MODULES][QR_MAX_MODULES];
} splash_qr_t;

// esp_qrcode_generate hands the code to a callback with no context argument
static splash_qr_t *s_qr_target;

/**
 * Check if a pixel is inside a rounded rectangle.
 * (x, y) relative to rect origin, (w, h) rect dimensions, r corner radius.
 */
static bool in_rounded_rect(int x, int y, int w, int h, int r)
{
    if (x < r && y < r) {
        int dx = r - x, dy = r - y;
        return dx * dx + dy * dy <= r * r;
    }
    if (x >= w - r && y < r) {
        int dx = x - (w - r - 1), dy = r - y;
        return dx * dx + dy * dy <= r * r;
    }
    if (x < r && y >= h - r) {
        int dx = r - x, dy = y - (h - r - 1);
        return dx * dx + dy * dy <= r * r;
    }
    if (x >= w - r && y >= h - r) {
        int dx = x - (w - r - 1), dy = y - (h - r - 1);
        return dx * dx + dy * dy <= r * r;
    }
    return true;
}

/**
 * QR code display callback: keeps the modules for drawing row by row.
 */
static void qr_capture_callback(esp_qrcode_handle_t qrcode)
{
    splash_qr_t *qr = s_qr_target;
    int size = esp_qrcode_get_size(qrcode);
    if (size > QR_MAX_MODULES)
        return;

    for (int qy = 0; qy < size; qy++) {
        for (int qx = 0; qx < size; qx++) {
            qr->modules[qy][qx] = esp_qrcode_get_module(qrcode, qx, qy) ? 1 : 0;
        }
    }
    qr->size = size;
}

static esp_err_t splash_qr_generate(splash_qr_t *qr, const char *text)
{
    s_qr_target = qr;
    qr->size = 0;

    esp_qrcode_config_t qr_cfg = {
        .display_func = qr_capture_callback,
        .max_qrcode_version = QR_MAX_VERSION,
        .qrcode_ecc_level = ESP_QRCODE_ECC_MED,
    };
    esp_err_t ret = esp_qrcode_generate(&qr_cfg, text);
    s_qr_target = NULL;
    if (ret == ESP_OK && qr->size == 0) {
        ret = ESP_FAIL;
    }
    return ret;
}

/**
 * Draw frame row y of the QR code: a white rounded card filling the SVG
 * placeholder area, then the modules on top.
 */
static void qr_draw_row(const splash_qr_t *qr, int y, uint8_t *row)
{
    // Fill the exact SVG placeholder area white (target_size + 4px border, rounded)
    // This matches the SVG's <rect rx="4" width="target_size+8" height="target_size+8"/>
    int pad = 4;
    int rect_w = qr->target_size + pad * 2;
    int rect_h = qr->target_size + pad * 2;
    int ry = y - (qr->pos_y - pad);
    if (ry < 0 || ry >= rect_h)
        return;
    for (int x = 0; x < rect_w; x++) {
        if (in_rounded_rect(x, ry, rect_w, rect_h, pad)) {
            set_row_pixel(row, qr->pos_x - pad + x, splash_white_nibble());
        }
    }

    // Use fractional module size so the QR content fills target_size exactly,
    // matching how the SVG app QR is rendered with fractional scaling.
    float module_f = (float) qr->target_size / qr->size;

    for (int qy = 0; qy < qr->size; qy++) {
        int py0 = qr->pos_y + (int) (qy * module_f);
        int py1 = qr->pos_y + (int) ((qy + 1) * module_f);
        if (y < py0 || y >= py1)
            continue;
        for (int qx = 0; qx < qr->size; qx++) {
            if (!qr->modules[qy][qx])
                continue;

            int px0 = qr->pos_x + (int) (qx * module_f);
            int px1 = qr->pos_x + (int) ((qx + 1) * module_f);
            for (int dx = px0; dx < px1; dx++) {
                set_row_pixel(row, dx, EPD_BLACK);
            }
        }
    }
}

// An embedded EPDGZ screen inflated row by row, with a QR code drawn over it
typedef struct {
    z_stream strm;
    bool ended;  // no more image data; remaining rows stay white
    const splash_qr_t *qr;
} splash_rows_t;

static esp_err_t splash_fill_row(void *ctx, int y, uint8_t *row)
{
    splash_rows_t *sr = (splash_rows_t *) ctx;
    size_t row_bytes = ((size_t) BOARD_HAL_DISPLAY_WIDTH + 1) / 2;

    // White where the image data falls short
    memset(row, splash_white_nibble() * 0x11, row_bytes);

    if (!sr->ended) {
        sr->strm.next_out = row;
        sr->strm.avail_out = row_bytes;
        while (sr->strm.avail_out > 0) {
            int ret = inflate(&sr->strm, Z_NO_FLUSH);
            if (ret == Z_STREAM_END) {
                sr->ended = true;
                break;
            }
            if (ret != Z_OK) {
                ESP_LOGE(TAG, "Decompression failed: %d", ret);
                return ESP_FAIL;
            }
        }
    }

    qr_draw_row(sr->qr, y, row);
    return ESP_OK;
}

// Show an embedded EPDGZ screen with a QR code encoding qr_text
static esp_err_t splash_show(const uint8_t *epdgz, size_t epdgz_size, const char *qr_text, int qr_x,
                             int qr_y, int qr_size)
{
    ESP_LOGI(TAG, "Generating QR code for: %s", qr_text);
    ESP_LOGI(TAG, "Drawing QR at (%d,%d) target %dpx", qr_x, qr_y, qr_size);

    // Too big for the stack; only one splash is ever on its way to the panel
    static splash_qr_t qr;
    qr.pos_x = qr_x;
    qr.pos_y = qr_y;
    qr.target_size = qr_size;
    esp_err_t ret = splash_qr_generate(&qr, qr_text);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to generate QR code");
        return ret;
    }

    splash_rows_t sr = {.qr = &qr};
    sr.strm.next_in = (Bytef *) epdgz;
    sr.strm.avail_in = epdgz_size;
    // 16 + MAX_WBITS enables gzip decoding
    if (inflateInit2(&sr.strm, 16 + MAX_WBITS) != Z_OK) {
        ESP_LOGE(TAG, "inflateInit2 failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Decompressing EPDGZ (%d bytes) to the display", (int) epdgz_size);
    ret = display_manager_show_rows(splash_fill_row, &sr);
    inflateEnd(&sr.strm);
    return ret;
}

esp_err_t splash_screen_display(void)
{
    ESP_LOGI(TAG, "Loading splash screen (%dx%d)", BOARD_HAL_DISPLAY_WIDTH,
             BOARD_HAL_DISPLAY_HEIGHT);

    // WiFi QR code for the device's unique hotspot SSID
    const char *ap_ssid = get_setup_ap_ssid();
    char wifi_qr_data[128];
    snprintf(wifi_qr_data, sizeof(wifi_qr_data), "WIFI:T:nopass;S:%s;;", ap_ssid);

    esp_err_t ret =
        splash_show(splash_epdgz_start, splash_epdgz_end - splash_epdgz_start, wifi_qr_data,
                    SPLASH_WIFI_QR_X, SPLASH_WIFI_QR_Y, SPLASH_WIFI_QR_SIZE);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Splash screen displayed successfully");
    }
    return ret;
}

esp_err_t splash_screen_display_setup_complete(const char *hostname)
{
    ESP_LOGI(TAG, "Showing setup complete screen (%dx%d)", BOARD_HAL_DISPLAY_WIDTH,
             BOARD_HAL_DISPLAY_HEIGHT);

    // QR code for the device's web interface URL
    char url[128];
    snprintf(url, sizeof(url), "http://%s.local", hostname);

    esp_err_t ret = splash_show(setup_complete_epdgz_start,
                                setup_complete_epdgz_end - setup_complete_epdgz_start, url,
                                SETUP_COMPLETE_QR_X, SETUP_COMPLETE_QR_Y, SETUP_COMPLETE_QR_SIZE);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Setup complete screen displayed successfully");
    }
    return ret;
}
