#include "image_processor.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "jpeg_decoder.h"

static const char *TAG = "image_processor";

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} rgb_t;

// Theoretical palette - used for BMP output (firmware compatibility)
static const rgb_t palette[7] = {
    {0, 0, 0},        // Black
    {255, 255, 255},  // White
    {255, 255, 0},    // Yellow
    {255, 0, 0},      // Red
    {0, 0, 0},        // Reserved
    {0, 0, 255},      // Blue
    {0, 255, 0}       // Green
};

// Measured palette - actual displayed colors from e-paper (used for dithering)
static const rgb_t palette_measured[7] = {
    {2, 2, 2},        // Black (measured)
    {190, 190, 190},  // White (measured - much darker than theoretical!)
    {205, 202, 0},    // Yellow (measured)
    {135, 19, 0},     // Red (measured - much darker)
    {0, 0, 0},        // Reserved
    {5, 64, 158},     // Blue (measured - much darker)
    {39, 102, 60}     // Green (measured - much darker)
};

static int find_closest_color(uint8_t r, uint8_t g, uint8_t b, const rgb_t *pal)
{
    int min_dist = INT_MAX;
    int closest = 1;

    for (int i = 0; i < 7; i++) {
        if (i == 4)
            continue;

        int dr = r - pal[i].r;
        int dg = g - pal[i].g;
        int db = b - pal[i].b;
        int dist = dr * dr + dg * dg + db * db;

        if (dist < min_dist) {
            min_dist = dist;
            closest = i;
        }
    }

    return closest;
}

static void apply_floyd_steinberg_dither(uint8_t *image, int width, int height,
                                         const rgb_t *dither_palette)
{
    int *errors = (int *) heap_caps_calloc(width * height * 3, sizeof(int), MALLOC_CAP_SPIRAM);
    if (!errors) {
        ESP_LOGE(TAG, "Failed to allocate error buffer");
        return;
    }

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int idx = (y * width + x) * 3;

            int old_r = image[idx] + errors[idx];
            int old_g = image[idx + 1] + errors[idx + 1];
            int old_b = image[idx + 2] + errors[idx + 2];

            old_r = (old_r < 0) ? 0 : (old_r > 255) ? 255 : old_r;
            old_g = (old_g < 0) ? 0 : (old_g > 255) ? 255 : old_g;
            old_b = (old_b < 0) ? 0 : (old_b > 255) ? 255 : old_b;

            // Find closest color using specified dither palette
            int color_idx = find_closest_color(old_r, old_g, old_b, dither_palette);

            // Output using theoretical palette (for BMP/firmware compatibility)
            image[idx] = palette[color_idx].r;
            image[idx + 1] = palette[color_idx].g;
            image[idx + 2] = palette[color_idx].b;

            // Calculate error using specified dither palette (for error diffusion)
            int err_r = old_r - dither_palette[color_idx].r;
            int err_g = old_g - dither_palette[color_idx].g;
            int err_b = old_b - dither_palette[color_idx].b;

            if (x + 1 < width) {
                int next_idx = idx + 3;
                errors[next_idx] += err_r * 7 / 16;
                errors[next_idx + 1] += err_g * 7 / 16;
                errors[next_idx + 2] += err_b * 7 / 16;
            }

            if (y + 1 < height) {
                if (x > 0) {
                    int next_idx = ((y + 1) * width + (x - 1)) * 3;
                    errors[next_idx] += err_r * 3 / 16;
                    errors[next_idx + 1] += err_g * 3 / 16;
                    errors[next_idx + 2] += err_b * 3 / 16;
                }

                int next_idx = ((y + 1) * width + x) * 3;
                errors[next_idx] += err_r * 5 / 16;
                errors[next_idx + 1] += err_g * 5 / 16;
                errors[next_idx + 2] += err_b * 5 / 16;

                if (x + 1 < width) {
                    next_idx = ((y + 1) * width + (x + 1)) * 3;
                    errors[next_idx] += err_r * 1 / 16;
                    errors[next_idx + 1] += err_g * 1 / 16;
                    errors[next_idx + 2] += err_b * 1 / 16;
                }
            }
        }
    }

    free(errors);
}

esp_err_t image_processor_init(void)
{
    ESP_LOGI(TAG, "Image processor initialized");
    return ESP_OK;
}

static uint8_t *resize_image(uint8_t *src, int src_w, int src_h, int dst_w, int dst_h)
{
    uint8_t *dst = (uint8_t *) heap_caps_malloc(dst_w * dst_h * 3, MALLOC_CAP_SPIRAM);
    if (!dst) {
        ESP_LOGE(TAG, "Failed to allocate resize buffer");
        return NULL;
    }

    memset(dst, 255, dst_w * dst_h * 3);

    float scale = fminf((float) dst_w / src_w, (float) dst_h / src_h);
    int new_w = (int) (src_w * scale);
    int new_h = (int) (src_h * scale);
    int offset_x = (dst_w - new_w) / 2;
    int offset_y = (dst_h - new_h) / 2;

    for (int y = 0; y < new_h; y++) {
        for (int x = 0; x < new_w; x++) {
            int src_x = (int) (x / scale);
            int src_y = (int) (y / scale);

            if (src_x >= src_w)
                src_x = src_w - 1;
            if (src_y >= src_h)
                src_y = src_h - 1;

            int dst_idx = ((y + offset_y) * dst_w + (x + offset_x)) * 3;
            int src_idx = (src_y * src_w + src_x) * 3;

            dst[dst_idx] = src[src_idx];
            dst[dst_idx + 1] = src[src_idx + 1];
            dst[dst_idx + 2] = src[src_idx + 2];
        }
    }

    return dst;
}

static esp_err_t write_bmp_file(const char *filename, uint8_t *rgb_data, int width, int height)
{
    FILE *fp = fopen(filename, "wb");
    if (!fp) {
        ESP_LOGE(TAG, "Failed to open file for writing: %s", filename);
        return ESP_FAIL;
    }

    int row_size = ((width * 3 + 3) / 4) * 4;
    int image_size = row_size * height;
    int file_size = 54 + image_size;

    uint8_t bmp_header[54] = {'B',
                              'M',
                              file_size & 0xFF,
                              (file_size >> 8) & 0xFF,
                              (file_size >> 16) & 0xFF,
                              (file_size >> 24) & 0xFF,
                              0,
                              0,
                              0,
                              0,
                              54,
                              0,
                              0,
                              0,
                              40,
                              0,
                              0,
                              0,
                              width & 0xFF,
                              (width >> 8) & 0xFF,
                              (width >> 16) & 0xFF,
                              (width >> 24) & 0xFF,
                              height & 0xFF,
                              (height >> 8) & 0xFF,
                              (height >> 16) & 0xFF,
                              (height >> 24) & 0xFF,
                              1,
                              0,
                              24,
                              0,
                              0,
                              0,
                              0,
                              0,
                              image_size & 0xFF,
                              (image_size >> 8) & 0xFF,
                              (image_size >> 16) & 0xFF,
                              (image_size >> 24) & 0xFF,
                              0x13,
                              0x0B,
                              0,
                              0,
                              0x13,
                              0x0B,
                              0,
                              0,
                              0,
                              0,
                              0,
                              0,
                              0,
                              0,
                              0,
                              0};

    fwrite(bmp_header, 1, 54, fp);

    uint8_t *row_buffer = (uint8_t *) malloc(row_size);
    if (!row_buffer) {
        fclose(fp);
        return ESP_FAIL;
    }

    for (int y = height - 1; y >= 0; y--) {
        memset(row_buffer, 0, row_size);
        for (int x = 0; x < width; x++) {
            int idx = (y * width + x) * 3;
            row_buffer[x * 3] = rgb_data[idx + 2];
            row_buffer[x * 3 + 1] = rgb_data[idx + 1];
            row_buffer[x * 3 + 2] = rgb_data[idx];
        }
        fwrite(row_buffer, 1, row_size, fp);
    }

    free(row_buffer);
    fclose(fp);

    return ESP_OK;
}

esp_err_t image_processor_convert_jpg_to_bmp(const char *jpg_path, const char *bmp_path,
                                             bool use_stock_mode)
{
    ESP_LOGI(TAG, "Converting %s to %s (mode: %s)", jpg_path, bmp_path,
             use_stock_mode ? "stock" : "enhanced");

    FILE *fp = fopen(jpg_path, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "Failed to open JPG file: %s", jpg_path);
        return ESP_FAIL;
    }

    fseek(fp, 0, SEEK_END);
    long jpg_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    uint8_t *jpg_buffer = (uint8_t *) heap_caps_malloc(jpg_size, MALLOC_CAP_SPIRAM);
    if (!jpg_buffer) {
        ESP_LOGE(TAG, "Failed to allocate JPG buffer");
        fclose(fp);
        return ESP_FAIL;
    }

    fread(jpg_buffer, 1, jpg_size, fp);
    fclose(fp);

    // First, get image info to know the output size
    esp_jpeg_image_cfg_t jpeg_cfg = {.indata = jpg_buffer,
                                     .indata_size = jpg_size,
                                     .outbuf = NULL,
                                     .outbuf_size = 0,
                                     .out_format = JPEG_IMAGE_FORMAT_RGB888,
                                     .out_scale = JPEG_IMAGE_SCALE_0,
                                     .flags = {
                                         .swap_color_bytes = 0,
                                     }};

    esp_jpeg_image_output_t outimg;
    esp_err_t ret = esp_jpeg_get_image_info(&jpeg_cfg, &outimg);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get JPEG info: %s", esp_err_to_name(ret));
        free(jpg_buffer);
        return ret;
    }

    ESP_LOGI(TAG, "JPEG info: %dx%d, output size: %zu bytes", outimg.width, outimg.height,
             outimg.output_len);

    // Allocate output buffer
    uint8_t *rgb_buffer = (uint8_t *) heap_caps_malloc(outimg.output_len, MALLOC_CAP_SPIRAM);
    if (!rgb_buffer) {
        ESP_LOGE(TAG, "Failed to allocate output buffer (%zu bytes)", outimg.output_len);
        free(jpg_buffer);
        return ESP_FAIL;
    }

    // Decode JPEG
    jpeg_cfg.outbuf = rgb_buffer;
    jpeg_cfg.outbuf_size = outimg.output_len;

    ret = esp_jpeg_decode(&jpeg_cfg, &outimg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "JPEG decode failed: %s", esp_err_to_name(ret));
        free(rgb_buffer);
        free(jpg_buffer);
        return ret;
    }

    ESP_LOGI(TAG, "Successfully decoded JPEG: %dx%d", outimg.width, outimg.height);
    free(jpg_buffer);

    uint8_t *resized = NULL;
    uint8_t *rotated = NULL;
    uint8_t *final_image = rgb_buffer;
    int final_width = outimg.width;
    int final_height = outimg.height;

    // Check if image is portrait and needs rotation for display
    bool is_portrait = outimg.height > outimg.width;
    if (is_portrait) {
        ESP_LOGI(TAG, "Portrait image detected (%dx%d), rotating 90° clockwise for display",
                 outimg.width, outimg.height);

        size_t rotated_size = outimg.width * outimg.height * 3;
        ESP_LOGI(TAG, "Allocating %zu bytes for rotation", rotated_size);

        rotated = (uint8_t *) heap_caps_malloc(rotated_size, MALLOC_CAP_SPIRAM);
        if (rotated) {
            ESP_LOGI(TAG, "Rotation buffer allocated, performing rotation");

            // Rotate 90° clockwise: swap dimensions and rotate pixels
            for (int y = 0; y < outimg.height; y++) {
                for (int x = 0; x < outimg.width; x++) {
                    int src_idx = (y * outimg.width + x) * 3;
                    // Rotate 90° clockwise: (x,y) -> (height-1-y, x)
                    int dst_x = outimg.height - 1 - y;
                    int dst_y = x;
                    int dst_idx = (dst_y * outimg.height + dst_x) * 3;

                    rotated[dst_idx] = rgb_buffer[src_idx];
                    rotated[dst_idx + 1] = rgb_buffer[src_idx + 1];
                    rotated[dst_idx + 2] = rgb_buffer[src_idx + 2];
                }
            }

            ESP_LOGI(TAG, "Rotation complete");
            final_image = rotated;
            final_width = outimg.height;  // Swapped
            final_height = outimg.width;  // Swapped
        } else {
            ESP_LOGE(TAG, "Failed to allocate rotation buffer, using original orientation");
        }
    }

    // Image should already be correct size from webapp (800x480 or 480x800 after rotation)
    // Only resize if dimensions don't match display
    if (final_width != DISPLAY_WIDTH || final_height != DISPLAY_HEIGHT) {
        ESP_LOGW(TAG, "Unexpected dimensions %dx%d, expected %dx%d", final_width, final_height,
                 DISPLAY_WIDTH, DISPLAY_HEIGHT);
        ESP_LOGI(TAG, "Resizing from %dx%d to %dx%d", final_width, final_height, DISPLAY_WIDTH,
                 DISPLAY_HEIGHT);
        resized =
            resize_image(final_image, final_width, final_height, DISPLAY_WIDTH, DISPLAY_HEIGHT);
        if (resized) {
            if (rotated)
                free(rotated);  // Free rotated buffer if we're replacing it
            final_image = resized;
            final_width = DISPLAY_WIDTH;
            final_height = DISPLAY_HEIGHT;
        } else {
            ESP_LOGW(TAG, "Resize failed, using current size");
        }
    }

    // Apply dithering based on processing mode
    // Stock mode: use theoretical palette (matches original Waveshare algorithm)
    // Enhanced mode: use measured palette (accurate error diffusion)
    const rgb_t *dither_palette = use_stock_mode ? palette : palette_measured;
    ESP_LOGI(TAG, "Applying Floyd-Steinberg dithering with %s palette",
             use_stock_mode ? "theoretical" : "measured");
    apply_floyd_steinberg_dither(final_image, final_width, final_height, dither_palette);

    // Write BMP file
    ESP_LOGI(TAG, "Writing BMP file");
    ret = write_bmp_file(bmp_path, final_image, final_width, final_height);

    // Cleanup
    if (resized) {
        free(resized);
    } else if (rotated) {
        free(rotated);
    }
    free(rgb_buffer);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Successfully converted %s to %s", jpg_path, bmp_path);
    } else {
        ESP_LOGE(TAG, "Failed to write BMP file");
    }

    return ret;
}
