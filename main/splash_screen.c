#include "splash_screen.h"

#include <stdio.h>
#include <string.h>
#include <zlib.h>

#include "board_hal.h"
#include "epaper.h"
#include "esp_heap_caps.h"
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

/**
 * Set a pixel in the 4-bit-per-pixel e-paper buffer.
 * Two pixels per byte: high nibble = even pixel, low nibble = odd pixel.
 */
static void set_pixel(uint8_t *buffer, int width, int x, int y, uint8_t color)
{
    if (x < 0 || x >= width || y < 0 || y >= BOARD_HAL_DISPLAY_HEIGHT)
        return;

    int byte_idx = (y * width + x) / 2;
    if (x % 2 == 0) {
        buffer[byte_idx] = (buffer[byte_idx] & 0x0F) | (color << 4);
    } else {
        buffer[byte_idx] = (buffer[byte_idx] & 0xF0) | (color & 0x0F);
    }
}

// Context passed to the QR code display callback
typedef struct {
    uint8_t *buffer;
    int buf_width;
    int pos_x;
    int pos_y;
    int target_size;
} qr_draw_ctx_t;

static qr_draw_ctx_t s_qr_draw_ctx;

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
 * QR code display callback — draws the QR code onto the e-paper buffer.
 */
static void qr_draw_callback(esp_qrcode_handle_t qrcode)
{
    qr_draw_ctx_t *ctx = &s_qr_draw_ctx;
    int qr_size = esp_qrcode_get_size(qrcode);

    // Fill the exact SVG placeholder area white (target_size + 4px border, rounded)
    // This matches the SVG's <rect rx="4" width="target_size+8" height="target_size+8"/>
    int pad = 4;
    int rect_w = ctx->target_size + pad * 2;
    int rect_h = ctx->target_size + pad * 2;
    for (int y = 0; y < rect_h; y++) {
        for (int x = 0; x < rect_w; x++) {
            if (in_rounded_rect(x, y, rect_w, rect_h, pad)) {
                set_pixel(ctx->buffer, ctx->buf_width, ctx->pos_x - pad + x, ctx->pos_y - pad + y,
                          EPD_WHITE);
            }
        }
    }

    // Use fractional module size so the QR content fills target_size exactly,
    // matching how the SVG app QR is rendered with fractional scaling.
    float module_f = (float) ctx->target_size / qr_size;

    for (int qy = 0; qy < qr_size; qy++) {
        int py0 = ctx->pos_y + (int) (qy * module_f);
        int py1 = ctx->pos_y + (int) ((qy + 1) * module_f);
        for (int qx = 0; qx < qr_size; qx++) {
            if (!esp_qrcode_get_module(qrcode, qx, qy))
                continue;

            int px0 = ctx->pos_x + (int) (qx * module_f);
            int px1 = ctx->pos_x + (int) ((qx + 1) * module_f);
            for (int dy = py0; dy < py1; dy++) {
                for (int dx = px0; dx < px1; dx++) {
                    set_pixel(ctx->buffer, ctx->buf_width, dx, dy, EPD_BLACK);
                }
            }
        }
    }
}

/**
 * Decompress gzipped data from memory buffer.
 * Returns ESP_OK on success.
 */
static esp_err_t decompress_gzip(const uint8_t *compressed, size_t compressed_size, uint8_t *output,
                                 size_t output_size)
{
    z_stream strm = {0};
    strm.avail_in = compressed_size;
    strm.next_in = (Bytef *) compressed;
    strm.avail_out = output_size;
    strm.next_out = output;

    // 16 + MAX_WBITS enables gzip decoding
    if (inflateInit2(&strm, 16 + MAX_WBITS) != Z_OK) {
        ESP_LOGE(TAG, "inflateInit2 failed");
        return ESP_FAIL;
    }

    int ret = inflate(&strm, Z_FINISH);
    inflateEnd(&strm);

    if (ret != Z_STREAM_END && ret != Z_OK) {
        ESP_LOGE(TAG, "Decompression failed: %d", ret);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Decompressed %d -> %d bytes", (int) compressed_size,
             (int) (output_size - strm.avail_out));
    return ESP_OK;
}

esp_err_t splash_screen_display(void)
{
    int width = BOARD_HAL_DISPLAY_WIDTH;
    int height = BOARD_HAL_DISPLAY_HEIGHT;
    int buf_size = ((width + 1) / 2) * height;

    ESP_LOGI(TAG, "Loading splash screen (%dx%d)", width, height);

    // Allocate buffer for the e-paper image
    uint8_t *epd_buffer = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!epd_buffer) {
        ESP_LOGE(TAG, "Failed to allocate display buffer");
        return ESP_ERR_NO_MEM;
    }

    // Fill with white initially
    memset(epd_buffer, 0x11, buf_size);  // 0x11 = white|white (index 1)

    // Decompress the embedded EPDGZ into the buffer
    size_t epdgz_size = splash_epdgz_end - splash_epdgz_start;
    ESP_LOGI(TAG, "Decompressing EPDGZ (%d bytes)", (int) epdgz_size);

    esp_err_t ret = decompress_gzip(splash_epdgz_start, epdgz_size, epd_buffer, buf_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to decompress splash EPDGZ");
        heap_caps_free(epd_buffer);
        return ret;
    }

    // Generate WiFi QR code for the device's unique hotspot SSID
    const char *ap_ssid = get_setup_ap_ssid();
    char wifi_qr_data[128];
    snprintf(wifi_qr_data, sizeof(wifi_qr_data), "WIFI:T:nopass;S:%s;;", ap_ssid);

    ESP_LOGI(TAG, "Generating WiFi QR code for: %s", ap_ssid);
    ESP_LOGI(TAG, "Drawing QR at (%d,%d) target %dpx", SPLASH_WIFI_QR_X, SPLASH_WIFI_QR_Y,
             SPLASH_WIFI_QR_SIZE);

    // Set up draw context for the callback
    s_qr_draw_ctx = (qr_draw_ctx_t){
        .buffer = epd_buffer,
        .buf_width = width,
        .pos_x = SPLASH_WIFI_QR_X,
        .pos_y = SPLASH_WIFI_QR_Y,
        .target_size = SPLASH_WIFI_QR_SIZE,
    };

    esp_qrcode_config_t qr_cfg = {
        .display_func = qr_draw_callback,
        .max_qrcode_version = 10,
        .qrcode_ecc_level = ESP_QRCODE_ECC_MED,
    };

    ret = esp_qrcode_generate(&qr_cfg, wifi_qr_data);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to generate WiFi QR code");
        heap_caps_free(epd_buffer);
        return ret;
    }

    // Display on e-paper
    ESP_LOGI(TAG, "Displaying splash screen");
    epaper_display(epd_buffer);

    heap_caps_free(epd_buffer);
    ESP_LOGI(TAG, "Splash screen displayed successfully");
    return ESP_OK;
}

esp_err_t splash_screen_display_setup_complete(const char *hostname)
{
    int width = BOARD_HAL_DISPLAY_WIDTH;
    int height = BOARD_HAL_DISPLAY_HEIGHT;
    int buf_size = ((width + 1) / 2) * height;

    ESP_LOGI(TAG, "Showing setup complete screen (%dx%d)", width, height);

    uint8_t *epd_buffer = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!epd_buffer) {
        ESP_LOGE(TAG, "Failed to allocate display buffer");
        return ESP_ERR_NO_MEM;
    }

    memset(epd_buffer, 0x11, buf_size);

    // Decompress the setup-complete EPDGZ
    size_t epdgz_size = setup_complete_epdgz_end - setup_complete_epdgz_start;
    ESP_LOGI(TAG, "Decompressing setup_complete EPDGZ (%d bytes)", (int) epdgz_size);

    esp_err_t ret = decompress_gzip(setup_complete_epdgz_start, epdgz_size, epd_buffer, buf_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to decompress setup_complete EPDGZ");
        heap_caps_free(epd_buffer);
        return ret;
    }

    // Generate QR code for the device's web interface URL
    char url[128];
    snprintf(url, sizeof(url), "http://%s.local", hostname);

    ESP_LOGI(TAG, "Generating web UI QR code for: %s", url);

    s_qr_draw_ctx = (qr_draw_ctx_t){
        .buffer = epd_buffer,
        .buf_width = width,
        .pos_x = SETUP_COMPLETE_QR_X,
        .pos_y = SETUP_COMPLETE_QR_Y,
        .target_size = SETUP_COMPLETE_QR_SIZE,
    };

    esp_qrcode_config_t qr_cfg = {
        .display_func = qr_draw_callback,
        .max_qrcode_version = 10,
        .qrcode_ecc_level = ESP_QRCODE_ECC_MED,
    };

    ret = esp_qrcode_generate(&qr_cfg, url);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to generate web UI QR code");
        heap_caps_free(epd_buffer);
        return ret;
    }

    ESP_LOGI(TAG, "Displaying setup complete screen");
    epaper_display(epd_buffer);

    heap_caps_free(epd_buffer);
    ESP_LOGI(TAG, "Setup complete screen displayed successfully");
    return ESP_OK;
}

esp_err_t splash_screen_display_ap_mode(const char *ssid, const char *password)
{
    // Interim implementation: reuse the OOBE splash background and place a
    // WPA2-formatted WiFi-join QR in the same slot. The dedicated dual-QR
    // template (with copy text and a separate URL-QR for 192.168.4.1) is
    // a follow-up — needs new SVG assets in scripts/generate_splash.py.
    if (!ssid || !password) {
        return ESP_ERR_INVALID_ARG;
    }

    int width = BOARD_HAL_DISPLAY_WIDTH;
    int height = BOARD_HAL_DISPLAY_HEIGHT;
    int buf_size = ((width + 1) / 2) * height;

    ESP_LOGI(TAG, "Loading AP-mode splash (%dx%d) — SSID=%s", width, height, ssid);

    uint8_t *epd_buffer = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!epd_buffer) {
        ESP_LOGE(TAG, "Failed to allocate display buffer");
        return ESP_ERR_NO_MEM;
    }

    memset(epd_buffer, 0x11, buf_size);

    size_t epdgz_size = splash_epdgz_end - splash_epdgz_start;
    esp_err_t ret = decompress_gzip(splash_epdgz_start, epdgz_size, epd_buffer, buf_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to decompress splash EPDGZ");
        heap_caps_free(epd_buffer);
        return ret;
    }

    char wifi_qr_data[160];
    snprintf(wifi_qr_data, sizeof(wifi_qr_data), "WIFI:T:WPA;S:%s;P:%s;;", ssid, password);

    s_qr_draw_ctx = (qr_draw_ctx_t){
        .buffer = epd_buffer,
        .buf_width = width,
        .pos_x = SPLASH_WIFI_QR_X,
        .pos_y = SPLASH_WIFI_QR_Y,
        .target_size = SPLASH_WIFI_QR_SIZE,
    };

    esp_qrcode_config_t qr_cfg = {
        .display_func = qr_draw_callback,
        .max_qrcode_version = 10,
        .qrcode_ecc_level = ESP_QRCODE_ECC_MED,
    };

    ret = esp_qrcode_generate(&qr_cfg, wifi_qr_data);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to generate WPA2 WiFi QR code");
        heap_caps_free(epd_buffer);
        return ret;
    }

    epaper_display(epd_buffer);

    heap_caps_free(epd_buffer);
    ESP_LOGI(TAG, "AP-mode splash displayed");
    return ESP_OK;
}
