#include "image_processor.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board_hal.h"
#include "color_palette.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "jpeg_decoder.h"

static const char *TAG = "image_processor";

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} rgb_t;

typedef struct {
    int dx;
    int dy;
    int numerator;
    int denominator;
} error_diffusion_t;

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

// Measured palette - loaded from config or defaults via color_palette module
static rgb_t palette_measured[7];

static esp_err_t load_calibrated_palette(void)
{
    color_palette_t palette;
    esp_err_t err = color_palette_load(&palette);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load palette: %s", esp_err_to_name(err));
        return err;
    }

    // Update palette_measured array with loaded values (stored or defaults)
    palette_measured[0] = (rgb_t){palette.black.r, palette.black.g, palette.black.b};
    palette_measured[1] = (rgb_t){palette.white.r, palette.white.g, palette.white.b};
    palette_measured[2] = (rgb_t){palette.yellow.r, palette.yellow.g, palette.yellow.b};
    palette_measured[3] = (rgb_t){palette.red.r, palette.red.g, palette.red.b};
    palette_measured[5] = (rgb_t){palette.blue.r, palette.blue.g, palette.blue.b};
    palette_measured[6] = (rgb_t){palette.green.r, palette.green.g, palette.green.b};

    return ESP_OK;
}

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

static void apply_error_diffusion_dither(uint8_t *image, int width, int height,
                                         const rgb_t *dither_palette, dither_algorithm_t algorithm)
{
    // Use three scanlines for error diffusion (current, next, and next+1 row)
    // This supports algorithms like Stucki and Sierra that diffuse to dy=2
    // Memory usage: ~15KB for 800x480 (3 rows * 800 pixels * 3 channels * 4 bytes)
    int *curr_errors = (int *) heap_caps_calloc(width * 3, sizeof(int), MALLOC_CAP_SPIRAM);
    int *next_errors = (int *) heap_caps_calloc(width * 3, sizeof(int), MALLOC_CAP_SPIRAM);
    int *next2_errors = (int *) heap_caps_calloc(width * 3, sizeof(int), MALLOC_CAP_SPIRAM);

    if (!curr_errors || !next_errors || !next2_errors) {
        ESP_LOGE(TAG, "Failed to allocate error buffers");
        if (curr_errors)
            free(curr_errors);
        if (next_errors)
            free(next_errors);
        if (next2_errors)
            free(next2_errors);
        return;
    }

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int img_idx = (y * width + x) * 3;
            int err_idx = x * 3;

            int old_r = image[img_idx] + curr_errors[err_idx];
            int old_g = image[img_idx + 1] + curr_errors[err_idx + 1];
            int old_b = image[img_idx + 2] + curr_errors[err_idx + 2];

            old_r = (old_r < 0) ? 0 : (old_r > 255) ? 255 : old_r;
            old_g = (old_g < 0) ? 0 : (old_g > 255) ? 255 : old_g;
            old_b = (old_b < 0) ? 0 : (old_b > 255) ? 255 : old_b;

            // Find closest color using specified dither palette
            int color_idx = find_closest_color(old_r, old_g, old_b, dither_palette);

            // Output using theoretical palette (for BMP/firmware compatibility)
            image[img_idx] = palette[color_idx].r;
            image[img_idx + 1] = palette[color_idx].g;
            image[img_idx + 2] = palette[color_idx].b;

            // Calculate error using specified dither palette (for error diffusion)
            int err_r = old_r - dither_palette[color_idx].r;
            int err_g = old_g - dither_palette[color_idx].g;
            int err_b = old_b - dither_palette[color_idx].b;

            // Define error diffusion matrices for different algorithms
            // Format: {dx, dy, numerator, denominator}
            const error_diffusion_t *matrix;
            int matrix_size;

            static const error_diffusion_t floyd_steinberg[] = {
                {1, 0, 7, 16}, {-1, 1, 3, 16}, {0, 1, 5, 16}, {1, 1, 1, 16}};
            static const error_diffusion_t stucki[] = {
                {1, 0, 8, 42},  {2, 0, 4, 42}, {-2, 1, 2, 42}, {-1, 1, 4, 42},
                {0, 1, 8, 42},  {1, 1, 4, 42}, {2, 1, 2, 42},  {-2, 2, 1, 42},
                {-1, 2, 2, 42}, {0, 2, 4, 42}, {1, 2, 2, 42},  {2, 2, 1, 42}};
            static const error_diffusion_t burkes[] = {
                {1, 0, 8, 32}, {2, 0, 4, 32}, {-2, 1, 2, 32}, {-1, 1, 4, 32},
                {0, 1, 8, 32}, {1, 1, 4, 32}, {2, 1, 2, 32}};
            static const error_diffusion_t sierra[] = {
                {1, 0, 5, 32}, {2, 0, 3, 32}, {-2, 1, 2, 32}, {-1, 1, 4, 32}, {0, 1, 5, 32},
                {1, 1, 4, 32}, {2, 1, 2, 32}, {-1, 2, 2, 32}, {0, 2, 3, 32},  {1, 2, 2, 32}};

            switch (algorithm) {
            case DITHER_STUCKI:
                matrix = stucki;
                matrix_size = sizeof(stucki) / sizeof(error_diffusion_t);
                break;
            case DITHER_BURKES:
                matrix = burkes;
                matrix_size = sizeof(burkes) / sizeof(error_diffusion_t);
                break;
            case DITHER_SIERRA:
                matrix = sierra;
                matrix_size = sizeof(sierra) / sizeof(error_diffusion_t);
                break;
            case DITHER_FLOYD_STEINBERG:
            default:
                matrix = floyd_steinberg;
                matrix_size = sizeof(floyd_steinberg) / sizeof(error_diffusion_t);
                break;
            }

            // Distribute error to neighboring pixels using selected algorithm
            for (int i = 0; i < matrix_size; i++) {
                int nx = x + matrix[i].dx;
                int ny = y + matrix[i].dy;

                if (nx >= 0 && nx < width && ny >= 0 && ny < height) {
                    int *target_errors;
                    if (ny == y) {
                        target_errors = curr_errors;  // Same row (dy=0)
                    } else if (ny == y + 1) {
                        target_errors = next_errors;  // Next row (dy=1)
                    } else if (ny == y + 2) {
                        target_errors = next2_errors;  // Two rows down (dy=2)
                    } else {
                        continue;  // Skip if beyond our buffer range
                    }

                    int target_idx = nx * 3;
                    target_errors[target_idx] +=
                        err_r * matrix[i].numerator / matrix[i].denominator;
                    target_errors[target_idx + 1] +=
                        err_g * matrix[i].numerator / matrix[i].denominator;
                    target_errors[target_idx + 2] +=
                        err_b * matrix[i].numerator / matrix[i].denominator;
                }
            }
        }

        // Rotate error buffers for next row
        int *temp = curr_errors;
        curr_errors = next_errors;
        next_errors = next2_errors;
        next2_errors = temp;
        memset(next2_errors, 0, width * 3 * sizeof(int));
    }

    free(curr_errors);
    free(next_errors);
    free(next2_errors);
}

esp_err_t image_processor_init(void)
{
    load_calibrated_palette();
    ESP_LOGI(TAG, "Image processor initialized");
    return ESP_OK;
}

esp_err_t image_processor_reload_palette(void)
{
    esp_err_t err = load_calibrated_palette();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to reload calibrated palette");
        return err;
    }
    ESP_LOGI(TAG, "Calibrated palette reloaded");
    return ESP_OK;
}

static uint8_t *resize_image(uint8_t *src, int src_w, int src_h, int dst_w, int dst_h)
{
    uint8_t *dst = (uint8_t *) heap_caps_malloc(dst_w * dst_h * 3, MALLOC_CAP_SPIRAM);
    if (!dst) {
        ESP_LOGE(TAG, "Failed to allocate resize buffer");
        return NULL;
    }

    // Cover mode: scale to fill entire display, crop excess
    // Use max scale to ensure image covers the entire display
    float scale_x = (float) dst_w / src_w;
    float scale_y = (float) dst_h / src_h;
    float scale = fmaxf(scale_x, scale_y);

    int scaled_w = (int) (src_w * scale);
    int scaled_h = (int) (src_h * scale);

    // Calculate crop offsets in scaled space to center the image
    int offset_x = (scaled_w - dst_w) / 2;
    int offset_y = (scaled_h - dst_h) / 2;

    ESP_LOGI(TAG, "Cover mode resize: %dx%d -> scale %.2f -> %dx%d, offset (%d,%d)", src_w, src_h,
             scale, scaled_w, scaled_h, offset_x, offset_y);

    for (int y = 0; y < dst_h; y++) {
        for (int x = 0; x < dst_w; x++) {
            // Map destination pixel to scaled space, then back to source
            float scaled_x = x + offset_x;
            float scaled_y = y + offset_y;

            // Map from scaled space to source space
            float src_x_f = scaled_x / scale;
            float src_y_f = scaled_y / scale;

            int src_x = (int) src_x_f;
            int src_y = (int) src_y_f;

            // Clamp to source bounds
            if (src_x >= src_w)
                src_x = src_w - 1;
            if (src_y >= src_h)
                src_y = src_h - 1;
            if (src_x < 0)
                src_x = 0;
            if (src_y < 0)
                src_y = 0;

            int dst_idx = (y * dst_w + x) * 3;
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
                                             bool use_stock_mode,
                                             dither_algorithm_t dither_algorithm)
{
    const char *algo_names[] = {"floyd-steinberg", "stucki", "burkes", "sierra"};
    ESP_LOGI(TAG, "Converting %s to %s (mode: %s, dither: %s)", jpg_path, bmp_path,
             use_stock_mode ? "stock" : "enhanced", algo_names[dither_algorithm]);

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

    // First, get image info at full scale to determine if we need to scale during decode
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

    // Determine optimal JPEG decode scale to reduce memory usage
    // Scale down large images during decode to avoid memory allocation failures
    esp_jpeg_image_scale_t decode_scale = JPEG_IMAGE_SCALE_0;
    int scaled_width = outimg.width;
    int scaled_height = outimg.height;

    // If image is much larger than display, use JPEG decoder's built-in scaling
    // This reduces memory usage significantly (e.g., 1/2 scale = 1/4 memory)
    if (outimg.width > BOARD_HAL_DISPLAY_WIDTH * 2 ||
        outimg.height > BOARD_HAL_DISPLAY_HEIGHT * 2) {
        decode_scale = JPEG_IMAGE_SCALE_1_2;  // 1:2 scale
        scaled_width = outimg.width / 2;
        scaled_height = outimg.height / 2;
        ESP_LOGI(TAG, "Image is large, using 1:2 JPEG decode scale: %dx%d -> %dx%d", outimg.width,
                 outimg.height, scaled_width, scaled_height);
    }

    if (outimg.width > BOARD_HAL_DISPLAY_WIDTH * 4 ||
        outimg.height > BOARD_HAL_DISPLAY_HEIGHT * 4) {
        decode_scale = JPEG_IMAGE_SCALE_1_4;  // 1:4 scale
        scaled_width = outimg.width / 4;
        scaled_height = outimg.height / 4;
        ESP_LOGI(TAG, "Image is very large, using 1:4 JPEG decode scale: %dx%d -> %dx%d",
                 outimg.width, outimg.height, scaled_width, scaled_height);
    }

    // Check if image is still too large even after maximum scaling
    // Maximum supported: 1:8 scale would be ~6400x3840 original -> 800x480 scaled
    if (outimg.width > BOARD_HAL_DISPLAY_WIDTH * 8 ||
        outimg.height > BOARD_HAL_DISPLAY_HEIGHT * 8) {
        ESP_LOGE(TAG, "Image is too large: %dx%d (max supported: %dx%d)", outimg.width,
                 outimg.height, BOARD_HAL_DISPLAY_WIDTH * 8, BOARD_HAL_DISPLAY_HEIGHT * 8);
        free(jpg_buffer);
        return ESP_ERR_INVALID_SIZE;
    }

    // Get scaled image info
    if (decode_scale != JPEG_IMAGE_SCALE_0) {
        jpeg_cfg.out_scale = decode_scale;
        ret = esp_jpeg_get_image_info(&jpeg_cfg, &outimg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to get scaled JPEG info: %s", esp_err_to_name(ret));
            free(jpg_buffer);
            return ret;
        }
        ESP_LOGI(TAG, "Scaled JPEG output: %dx%d, size: %zu bytes", outimg.width, outimg.height,
                 outimg.output_len);
    }

    // Final safety check: ensure decoded size won't exceed available memory
    // Typical SPIRAM: 8MB, need headroom for processing
    const size_t MAX_DECODED_SIZE = 4 * 1024 * 1024;  // 4MB max for decoded image
    if (outimg.output_len > MAX_DECODED_SIZE) {
        ESP_LOGE(TAG, "Decoded image size too large: %zu bytes (max: %zu bytes)", outimg.output_len,
                 MAX_DECODED_SIZE);
        free(jpg_buffer);
        return ESP_ERR_NO_MEM;
    }

    // Allocate output buffer for scaled image
    uint8_t *rgb_buffer = (uint8_t *) heap_caps_malloc(outimg.output_len, MALLOC_CAP_SPIRAM);
    if (!rgb_buffer) {
        ESP_LOGE(TAG, "Failed to allocate output buffer (%zu bytes)", outimg.output_len);
        free(jpg_buffer);
        return ESP_FAIL;
    }

    // Decode JPEG with scaling
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

    // Check if orientation mismatch requires rotation
    bool image_is_portrait = outimg.height > outimg.width;
    bool board_is_portrait = BOARD_HAL_DISPLAY_HEIGHT > BOARD_HAL_DISPLAY_WIDTH;
    bool needs_rotation = image_is_portrait != board_is_portrait;

    ESP_LOGI(TAG, "Orientation check: image_portrait=%d, board_portrait=%d, needs_rotation=%d",
             image_is_portrait, board_is_portrait, needs_rotation);

    // STEP 1: Resize to appropriate target size immediately to free large decoded buffer
    int target_width, target_height;

    if (needs_rotation) {
        // Mismatch: resize to fit display width/height after rotation (swap target dims)
        target_width = (outimg.width * BOARD_HAL_DISPLAY_WIDTH) / outimg.height;
        target_height = BOARD_HAL_DISPLAY_WIDTH;
        ESP_LOGI(TAG, "Orientation mismatch: resizing %dx%d -> %dx%d (will rotate after)",
                 final_width, final_height, target_width, target_height);
    } else {
        // Matches: resize directly to display size
        target_width = BOARD_HAL_DISPLAY_WIDTH;
        target_height = BOARD_HAL_DISPLAY_HEIGHT;
        ESP_LOGI(TAG, "Orientation matches: resizing %dx%d -> %dx%d", final_width, final_height,
                 target_width, target_height);
    }

    // Only resize if needed
    if (final_width != target_width || final_height != target_height) {
        resized = resize_image(final_image, final_width, final_height, target_width, target_height);
        if (!resized) {
            ESP_LOGE(TAG, "Failed to resize image from %dx%d to %dx%d", final_width, final_height,
                     target_width, target_height);
            free(rgb_buffer);
            return ESP_FAIL;
        }
        free(rgb_buffer);
        rgb_buffer = NULL;
        final_image = resized;
        final_width = target_width;
        final_height = target_height;
        ESP_LOGI(TAG, "Resize complete: %dx%d", final_width, final_height);
    }

    // STEP 2: Rotate if orientation mismatch detected (now working with smaller buffer)
    if (needs_rotation) {
        size_t rotated_size = final_width * final_height * 3;
        ESP_LOGI(TAG, "Rotating portrait image, allocating %zu bytes", rotated_size);

        rotated = (uint8_t *) heap_caps_malloc(rotated_size, MALLOC_CAP_SPIRAM);
        if (!rotated) {
            ESP_LOGE(TAG, "Failed to allocate rotation buffer (%zu bytes)", rotated_size);
            if (resized)
                free(resized);
            else if (rgb_buffer)
                free(rgb_buffer);
            return ESP_FAIL;
        }

        ESP_LOGI(TAG, "Performing 90° clockwise rotation");

        // Rotate 90° clockwise: swap dimensions and rotate pixels
        for (int y = 0; y < final_height; y++) {
            for (int x = 0; x < final_width; x++) {
                int src_idx = (y * final_width + x) * 3;
                int dst_x = final_height - 1 - y;
                int dst_y = x;
                int dst_idx = (dst_y * final_height + dst_x) * 3;

                rotated[dst_idx] = final_image[src_idx];
                rotated[dst_idx + 1] = final_image[src_idx + 1];
                rotated[dst_idx + 2] = final_image[src_idx + 2];
            }
        }

        ESP_LOGI(TAG, "Rotation complete");
        if (resized)
            free(resized);
        else if (rgb_buffer)
            free(rgb_buffer);
        rgb_buffer = NULL;
        resized = NULL;
        final_image = rotated;

        // Swap dimensions after rotation
        int temp = final_width;
        final_width = final_height;
        final_height = temp;
        ESP_LOGI(TAG, "After rotation: %dx%d", final_width, final_height);
    }

    // STEP 3: Final resize if still needed (shouldn't happen normally)
    if (final_width != BOARD_HAL_DISPLAY_WIDTH || final_height != BOARD_HAL_DISPLAY_HEIGHT) {
        ESP_LOGE(TAG, "Unexpected dimensions %dx%d after processing, expected %dx%d", final_width,
                 final_height, BOARD_HAL_DISPLAY_WIDTH, BOARD_HAL_DISPLAY_HEIGHT);
        uint8_t *final_resized = resize_image(final_image, final_width, final_height,
                                              BOARD_HAL_DISPLAY_WIDTH, BOARD_HAL_DISPLAY_HEIGHT);
        if (!final_resized) {
            ESP_LOGE(TAG, "Final resize failed from %dx%d to %dx%d", final_width, final_height,
                     BOARD_HAL_DISPLAY_WIDTH, BOARD_HAL_DISPLAY_HEIGHT);
            if (rotated)
                free(rotated);
            else if (resized)
                free(resized);
            else if (rgb_buffer)
                free(rgb_buffer);
            return ESP_FAIL;
        }
        if (rotated) {
            free(rotated);
            rotated = NULL;
        } else if (resized) {
            free(resized);
            resized = NULL;
        } else if (rgb_buffer) {
            free(rgb_buffer);
            rgb_buffer = NULL;
        }
        final_image = final_resized;
        final_width = BOARD_HAL_DISPLAY_WIDTH;
        final_height = BOARD_HAL_DISPLAY_HEIGHT;
    }

    // Apply dithering based on processing mode
    // Stock mode: use theoretical palette (matches original Waveshare algorithm)
    // Enhanced mode: use measured palette (accurate error diffusion)
    const rgb_t *dither_palette = use_stock_mode ? palette : palette_measured;

    // Use dithering algorithm directly (already an enum)
    ESP_LOGI(TAG, "Applying %s dithering with %s palette", algo_names[dither_algorithm],
             use_stock_mode ? "theoretical" : "measured");
    apply_error_diffusion_dither(final_image, final_width, final_height, dither_palette,
                                 dither_algorithm);

    // Write BMP file
    ESP_LOGI(TAG, "Writing BMP file");
    ret = write_bmp_file(bmp_path, final_image, final_width, final_height);

    // Cleanup - free final_image (which could be rotated, resized, final_resized, or rgb_buffer)
    free(final_image);

    // These should already be NULL if they were freed earlier, but check anyway
    if (rgb_buffer && rgb_buffer != final_image) {
        free(rgb_buffer);
    }
    if (resized && resized != final_image) {
        free(resized);
    }
    if (rotated && rotated != final_image) {
        free(rotated);
    }

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Successfully converted %s to %s", jpg_path, bmp_path);
    } else {
        ESP_LOGE(TAG, "Failed to write BMP file");
    }

    return ret;
}
