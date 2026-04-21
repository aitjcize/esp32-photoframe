#include "image_processor.h"

#include <limits.h>
#include <math.h>
#include <png.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board_hal.h"
#include "color_palette.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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

// Precomputed LUTs for sRGB <-> linear conversion
#define LINEAR_TO_SRGB_SIZE 4096
static float srgb_to_linear_lut[256];
static uint8_t linear_to_srgb_lut[LINEAR_TO_SRGB_SIZE];
static bool luts_initialized = false;

static void init_gamma_luts(void)
{
    if (luts_initialized) {
        return;
    }

    // sRGB byte -> linear float
    for (int i = 0; i < 256; i++) {
        float s = i / 255.0f;
        srgb_to_linear_lut[i] = s > 0.04045f ? powf((s + 0.055f) / 1.055f, 2.4f) : s / 12.92f;
    }

    // linear float (scaled to 0..4095) -> sRGB byte
    for (int i = 0; i < LINEAR_TO_SRGB_SIZE; i++) {
        float lin = (float) i / (LINEAR_TO_SRGB_SIZE - 1);
        float s = lin > 0.0031308f ? 1.055f * powf(lin, 1.0f / 2.4f) - 0.055f : 12.92f * lin;
        int v = (int) roundf(s * 255.0f);
        linear_to_srgb_lut[i] = (uint8_t) (v < 0 ? 0 : (v > 255 ? 255 : v));
    }

    luts_initialized = true;
}

static inline float srgb_to_linear(uint8_t v)
{
    return srgb_to_linear_lut[v];
}

static inline uint8_t linear_to_srgb(float lin)
{
    if (lin <= 0.0f)
        return 0;
    if (lin >= 1.0f)
        return 255;
    int idx = (int) (lin * (LINEAR_TO_SRGB_SIZE - 1) + 0.5f);
    return linear_to_srgb_lut[idx];
}

static void fast_compress_dynamic_range(uint8_t *image, int width, int height,
                                        const rgb_t *measured_palette)
{
    init_gamma_luts();

    // Compute display black/white luminance in linear space
    float black_Y = 0.2126729f * srgb_to_linear(measured_palette[0].r) +
                    0.7151522f * srgb_to_linear(measured_palette[0].g) +
                    0.0721750f * srgb_to_linear(measured_palette[0].b);
    float white_Y = 0.2126729f * srgb_to_linear(measured_palette[1].r) +
                    0.7151522f * srgb_to_linear(measured_palette[1].g) +
                    0.0721750f * srgb_to_linear(measured_palette[1].b);

    float range = white_Y - black_Y;

    ESP_LOGI(TAG, "Fast CDR: Display black Y=%.4f, white Y=%.4f (range: %.4f)", black_Y, white_Y,
             range);

    int total_pixels = width * height;
    for (int i = 0; i < total_pixels; i++) {
        int idx = i * 3;

        float lr = srgb_to_linear(image[idx]);
        float lg = srgb_to_linear(image[idx + 1]);
        float lb = srgb_to_linear(image[idx + 2]);

        // Original luminance
        float Y = 0.2126729f * lr + 0.7151522f * lg + 0.0721750f * lb;

        // Compressed luminance mapped to [black_Y, white_Y]
        float compressed_Y = black_Y + Y * range;

        // Scale RGB channels proportionally
        float scale;
        if (Y > 1e-6f) {
            scale = compressed_Y / Y;
        } else {
            // Near-black pixel: just set to display black level
            scale = 0.0f;
            lr = black_Y;
            lg = black_Y;
            lb = black_Y;
        }

        if (scale != 0.0f) {
            lr *= scale;
            lg *= scale;
            lb *= scale;
        }

        image[idx] = linear_to_srgb(lr);
        image[idx + 1] = linear_to_srgb(lg);
        image[idx + 2] = linear_to_srgb(lb);

        // Delay every 2000 pixels to allow IDLE task to feed watchdog
        if ((i % 2000) == 0) {
            vTaskDelay(1);
        }
    }
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

// Write PNG directly to file (efficient for file-based processing)
static esp_err_t write_png_file(const char *filename, uint8_t *rgb_data, int width, int height)
{
    FILE *fp = fopen(filename, "wb");
    if (!fp) {
        ESP_LOGE(TAG, "Failed to open file for writing: %s", filename);
        return ESP_FAIL;
    }

    png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png_ptr) {
        ESP_LOGE(TAG, "Failed to create PNG write struct");
        fclose(fp);
        return ESP_FAIL;
    }

    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        ESP_LOGE(TAG, "Failed to create PNG info struct");
        png_destroy_write_struct(&png_ptr, NULL);
        fclose(fp);
        return ESP_FAIL;
    }

    if (setjmp(png_jmpbuf(png_ptr))) {
        ESP_LOGE(TAG, "PNG encoding error");
        png_destroy_write_struct(&png_ptr, &info_ptr);
        fclose(fp);
        return ESP_FAIL;
    }

    png_init_io(png_ptr, fp);

    png_set_IHDR(png_ptr, info_ptr, width, height, 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);

    png_write_info(png_ptr, info_ptr);

    for (int y = 0; y < height; y++) {
        png_write_row(png_ptr, (png_bytep) &rgb_data[y * width * 3]);
    }

    png_write_end(png_ptr, NULL);
    png_destroy_write_struct(&png_ptr, &info_ptr);
    fclose(fp);

    return ESP_OK;
}

// Core processing function that takes decoded RGB buffer and processes it
// Returns processed RGB buffer (caller must free) and dimensions
static esp_err_t process_rgb_buffer_core(uint8_t *rgb_buffer, int width, int height,
                                         dither_algorithm_t dither_algorithm, uint8_t **out_buffer,
                                         int *out_width, int *out_height)
{
    ESP_LOGI(TAG, "Processing RGB buffer: %dx%d", width, height);

    uint8_t *resized = NULL;
    uint8_t *rotated = NULL;
    uint8_t *final_image = rgb_buffer;
    int final_width = width;
    int final_height = height;

    bool image_is_portrait = height > width;
    bool board_is_portrait = BOARD_HAL_DISPLAY_HEIGHT > BOARD_HAL_DISPLAY_WIDTH;
    bool needs_rotation = image_is_portrait != board_is_portrait;

    // STEP 1: Resize for target orientation
    int target_width, target_height;
    if (needs_rotation) {
        target_width = (width * BOARD_HAL_DISPLAY_WIDTH) / height;
        target_height = BOARD_HAL_DISPLAY_WIDTH;
    } else {
        target_width = BOARD_HAL_DISPLAY_WIDTH;
        target_height = BOARD_HAL_DISPLAY_HEIGHT;
    }

    if (final_width != target_width || final_height != target_height) {
        ESP_LOGI(TAG, "Resizing image to %dx%d", target_width, target_height);
        resized = resize_image(final_image, final_width, final_height, target_width, target_height);
        if (!resized) {
            ESP_LOGE(TAG, "Failed to resize image to %dx%d", target_width, target_height);
            return ESP_FAIL;
        }
        if (final_image != rgb_buffer)
            heap_caps_free(final_image);
        final_image = resized;
        final_width = target_width;
        final_height = target_height;
    }

    // STEP 2: Rotate
    if (needs_rotation) {
        ESP_LOGI(TAG, "Rotating image by 90 degrees");
        size_t rotated_size = final_width * final_height * 3;
        rotated = (uint8_t *) heap_caps_malloc(rotated_size, MALLOC_CAP_SPIRAM);
        if (!rotated) {
            ESP_LOGE(TAG, "Failed to allocate rotation buffer of %zu bytes", rotated_size);
            if (final_image != rgb_buffer)
                heap_caps_free(final_image);
            return ESP_FAIL;
        }

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
        if (final_image != rgb_buffer)
            heap_caps_free(final_image);
        final_image = rotated;
        int temp = final_width;
        final_width = final_height;
        final_height = temp;
    }

    // STEP 3: Final fit check
    if (final_width != BOARD_HAL_DISPLAY_WIDTH || final_height != BOARD_HAL_DISPLAY_HEIGHT) {
        uint8_t *final_resized = resize_image(final_image, final_width, final_height,
                                              BOARD_HAL_DISPLAY_WIDTH, BOARD_HAL_DISPLAY_HEIGHT);
        if (!final_resized) {
            ESP_LOGE(TAG, "Failed to final resize image to %dx%d", BOARD_HAL_DISPLAY_WIDTH,
                     BOARD_HAL_DISPLAY_HEIGHT);
            if (final_image != rgb_buffer)
                heap_caps_free(final_image);
            return ESP_FAIL;
        }
        if (final_image != rgb_buffer)
            heap_caps_free(final_image);
        final_image = final_resized;
        final_width = BOARD_HAL_DISPLAY_WIDTH;
        final_height = BOARD_HAL_DISPLAY_HEIGHT;
    }

    // Apply fast Compressed Dynamic Range (fast CDR)
    ESP_LOGI(TAG, "Applying fast Compressed Dynamic Range (fast CDR)");
    fast_compress_dynamic_range(final_image, final_width, final_height, palette_measured);

    // Apply Dithering (always use measured palette)
    apply_error_diffusion_dither(final_image, final_width, final_height, palette_measured,
                                 dither_algorithm);

    *out_buffer = final_image;
    *out_width = final_width;
    *out_height = final_height;
    return ESP_OK;
}

// Decode JPG from buffer to RGB
static esp_err_t decode_jpg_buffer(const uint8_t *jpg_data, size_t jpg_size, uint8_t **rgb_buffer,
                                   int *width, int *height)
{
    esp_jpeg_image_cfg_t jpeg_cfg = {.indata = (uint8_t *) jpg_data,
                                     .indata_size = jpg_size,
                                     .out_format = JPEG_IMAGE_FORMAT_RGB888,
                                     .out_scale = JPEG_IMAGE_SCALE_0};
    esp_jpeg_image_output_t outimg;
    esp_jpeg_get_image_info(&jpeg_cfg, &outimg);
    int original_width = outimg.width;
    int original_height = outimg.height;

    // Scaling logic - scale down large images to save memory
    if (outimg.width > BOARD_HAL_DISPLAY_WIDTH * 4 || outimg.height > BOARD_HAL_DISPLAY_HEIGHT * 4)
        jpeg_cfg.out_scale = JPEG_IMAGE_SCALE_1_4;
    else if (outimg.width > BOARD_HAL_DISPLAY_WIDTH * 2 ||
             outimg.height > BOARD_HAL_DISPLAY_HEIGHT * 2)
        jpeg_cfg.out_scale = JPEG_IMAGE_SCALE_1_2;

    if (jpeg_cfg.out_scale != JPEG_IMAGE_SCALE_0) {
        esp_jpeg_get_image_info(&jpeg_cfg, &outimg);
        ESP_LOGI(TAG, "JPG scaled from %dx%d to %dx%d (scale: 1/%d)", original_width,
                 original_height, outimg.width, outimg.height, 1 << jpeg_cfg.out_scale);
    } else {
        ESP_LOGI(TAG, "JPG size: %dx%d (no scaling needed)", outimg.width, outimg.height);
    }

    *rgb_buffer = (uint8_t *) heap_caps_malloc(outimg.output_len, MALLOC_CAP_SPIRAM);
    if (!*rgb_buffer) {
        ESP_LOGE(TAG, "Failed to allocate JPG RGB buffer of %u bytes", outimg.output_len);
        return ESP_ERR_NO_MEM;
    }

    jpeg_cfg.outbuf = *rgb_buffer;
    jpeg_cfg.outbuf_size = outimg.output_len;
    esp_err_t decode_err = esp_jpeg_decode(&jpeg_cfg, &outimg);
    if (decode_err != ESP_OK) {
        ESP_LOGE(TAG, "JPG decoding failed: %s", esp_err_to_name(decode_err));
        heap_caps_free(*rgb_buffer);
        *rgb_buffer = NULL;
        return ESP_FAIL;
    }

    *width = outimg.width;
    *height = outimg.height;
    return ESP_OK;
}

// PNG memory read callback structure
typedef struct {
    const uint8_t *data;
    size_t size;
    size_t offset;
} png_mem_read_t;

static void png_mem_read_callback(png_structp png_ptr, png_bytep data, png_size_t length)
{
    png_mem_read_t *mem = (png_mem_read_t *) png_get_io_ptr(png_ptr);
    if (mem->offset + length > mem->size) {
        png_error(png_ptr, "Read past end of buffer");
        return;
    }
    memcpy(data, mem->data + mem->offset, length);
    mem->offset += length;
}

// Decode PNG from buffer to RGB
static esp_err_t decode_png_buffer(const uint8_t *png_data, size_t png_size, uint8_t **rgb_buffer,
                                   int *width, int *height)
{
    png_mem_read_t mem = {.data = png_data, .size = png_size, .offset = 0};

    png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png_ptr) {
        ESP_LOGE(TAG, "Failed to create PNG read struct");
        return ESP_FAIL;
    }

    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        ESP_LOGE(TAG, "Failed to create PNG info struct");
        png_destroy_read_struct(&png_ptr, NULL, NULL);
        return ESP_FAIL;
    }

    if (setjmp(png_jmpbuf(png_ptr))) {
        ESP_LOGE(TAG, "PNG decoding error");
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        return ESP_FAIL;
    }

    png_set_read_fn(png_ptr, &mem, png_mem_read_callback);
    png_read_png(png_ptr, info_ptr,
                 PNG_TRANSFORM_STRIP_16 | PNG_TRANSFORM_PACKING | PNG_TRANSFORM_EXPAND |
                     PNG_TRANSFORM_STRIP_ALPHA,
                 NULL);

    *width = png_get_image_width(png_ptr, info_ptr);
    *height = png_get_image_height(png_ptr, info_ptr);
    ESP_LOGI(TAG, "PNG Image info: %dx%d", *width, *height);

    size_t rgb_size = (*width) * (*height) * 3;
    if (rgb_size > 6 * 1024 * 1024) {
        ESP_LOGE(TAG, "PNG image too large for memory: %zu bytes (limit 6MB)", rgb_size);
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        return ESP_ERR_NO_MEM;
    }

    *rgb_buffer = (uint8_t *) heap_caps_malloc(rgb_size, MALLOC_CAP_SPIRAM);
    if (!*rgb_buffer) {
        ESP_LOGE(TAG, "Failed to allocate PNG RGB buffer of %zu bytes", rgb_size);
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        return ESP_ERR_NO_MEM;
    }

    png_bytep *row_pointers = png_get_rows(png_ptr, info_ptr);
    int channels = png_get_channels(png_ptr, info_ptr);
    if (channels != 3) {
        ESP_LOGE(TAG, "Unsupported channel count: %d", channels);
        heap_caps_free(*rgb_buffer);
        *rgb_buffer = NULL;
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        return ESP_FAIL;
    }

    for (int y = 0; y < *height; y++) {
        memcpy(*rgb_buffer + y * (*width) * 3, row_pointers[y], (*width) * 3);
    }

    png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
    return ESP_OK;
}

image_format_t image_processor_detect_format_buffer(const uint8_t *data, size_t size)
{
    if (size < 8) {
        return IMAGE_FORMAT_UNKNOWN;
    }

    if (data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E && data[3] == 0x47 &&
        data[4] == 0x0D && data[5] == 0x0A && data[6] == 0x1A && data[7] == 0x0A) {
        return IMAGE_FORMAT_PNG;
    } else if (data[0] == 0x42 && data[1] == 0x4D) {
        return IMAGE_FORMAT_BMP;
    } else if (data[0] == 0xFF && data[1] == 0xD8) {
        return IMAGE_FORMAT_JPG;
    } else if (data[0] == 0x1F && data[1] == 0x8B) {
        return IMAGE_FORMAT_EPD_GZ;
    }

    return IMAGE_FORMAT_UNKNOWN;
}

esp_err_t image_processor_process_to_rgb(const uint8_t *input_data, size_t input_size,
                                         image_format_t format, dither_algorithm_t dither_algorithm,
                                         image_process_rgb_result_t *result)
{
    if (!input_data || input_size == 0 || !result) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *algo_names[] = {"floyd-steinberg", "stucki", "burkes", "sierra"};
    ESP_LOGI(TAG, "Processing buffer to RGB (%zu bytes, format: %d, dither: %s)", input_size,
             format, algo_names[dither_algorithm]);

    memset(result, 0, sizeof(*result));

    // Decode input to RGB
    uint8_t *rgb_buffer = NULL;
    int width = 0, height = 0;
    esp_err_t err;

    if (format == IMAGE_FORMAT_JPG) {
        err = decode_jpg_buffer(input_data, input_size, &rgb_buffer, &width, &height);
    } else if (format == IMAGE_FORMAT_PNG) {
        err = decode_png_buffer(input_data, input_size, &rgb_buffer, &width, &height);
    } else {
        ESP_LOGE(TAG, "Unsupported image format for buffer processing: %d", format);
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "Decoded image: %dx%d", width, height);

    // Process RGB buffer
    uint8_t *processed_buffer = NULL;
    int processed_width = 0, processed_height = 0;
    err = process_rgb_buffer_core(rgb_buffer, width, height, dither_algorithm, &processed_buffer,
                                  &processed_width, &processed_height);

    // Free original RGB buffer if it wasn't reused
    if (rgb_buffer != processed_buffer) {
        heap_caps_free(rgb_buffer);
    }

    if (err != ESP_OK) {
        return err;
    }

    // Return the processed RGB buffer directly (no PNG encoding)
    result->rgb_data = processed_buffer;
    result->rgb_size = processed_width * processed_height * 3;
    result->width = processed_width;
    result->height = processed_height;

    ESP_LOGI(TAG, "Processed to RGB buffer: %dx%d (%zu bytes)", processed_width, processed_height,
             result->rgb_size);
    return ESP_OK;
}

esp_err_t image_processor_process(const char *input_path, const char *output_path,
                                  dither_algorithm_t dither_algorithm)
{
    const char *algo_names[] = {"floyd-steinberg", "stucki", "burkes", "sierra"};
    ESP_LOGI(TAG, "Processing %s -> %s (dither: %s)", input_path, output_path,
             algo_names[dither_algorithm]);

    // Detect format first
    image_format_t format = image_processor_detect_format(input_path);
    if (format == IMAGE_FORMAT_UNKNOWN || format == IMAGE_FORMAT_BMP) {
        ESP_LOGE(TAG, "Unsupported image format for processing");
        return ESP_FAIL;
    }

    // Read entire file into buffer
    FILE *fp = fopen(input_path, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "Failed to open input file: %s", input_path);
        return ESP_FAIL;
    }

    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    uint8_t *file_buffer = heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM);
    if (!file_buffer) {
        ESP_LOGE(TAG, "Failed to allocate file buffer of %ld bytes", file_size);
        fclose(fp);
        return ESP_ERR_NO_MEM;
    }

    size_t read_bytes = fread(file_buffer, 1, file_size, fp);
    fclose(fp);

    if (read_bytes != file_size) {
        ESP_LOGE(TAG, "Failed to read entire file");
        heap_caps_free(file_buffer);
        return ESP_FAIL;
    }

    // Decode to RGB buffer
    uint8_t *rgb_buffer = NULL;
    int width = 0, height = 0;
    esp_err_t err;

    if (format == IMAGE_FORMAT_JPG) {
        err = decode_jpg_buffer(file_buffer, file_size, &rgb_buffer, &width, &height);
    } else if (format == IMAGE_FORMAT_PNG) {
        err = decode_png_buffer(file_buffer, file_size, &rgb_buffer, &width, &height);
    } else {
        heap_caps_free(file_buffer);
        return ESP_ERR_NOT_SUPPORTED;
    }

    // Free input file buffer immediately after decoding
    heap_caps_free(file_buffer);

    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "Decoded image: %dx%d", width, height);

    // Process RGB buffer
    uint8_t *processed_buffer = NULL;
    int processed_width = 0, processed_height = 0;
    err = process_rgb_buffer_core(rgb_buffer, width, height, dither_algorithm, &processed_buffer,
                                  &processed_width, &processed_height);

    // Free original RGB buffer if it wasn't reused
    if (rgb_buffer != processed_buffer) {
        heap_caps_free(rgb_buffer);
    }

    if (err != ESP_OK) {
        return err;
    }

    // Write directly to file using png_init_io (no intermediate buffer)
    ESP_LOGI(TAG, "Writing PNG output to %s", output_path);
    err = write_png_file(output_path, processed_buffer, processed_width, processed_height);

    heap_caps_free(processed_buffer);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Successfully wrote PNG to %s", output_path);
    }

    return err;
}

bool image_processor_is_processed(const char *input_path)
{
    ESP_LOGD(TAG, "Checking if image is already processed: %s", input_path);

    FILE *fp = fopen(input_path, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "Failed to open input file: %s", input_path);
        return false;
    }

    uint8_t sig[8];
    size_t read = fread(sig, 1, 8, fp);
    if (read != 8 || png_sig_cmp(sig, 0, 8) != 0) {
        ESP_LOGD(TAG, "Not a PNG file");
        fclose(fp);
        return false;
    }

    png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png_ptr) {
        fclose(fp);
        return false;
    }

    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        png_destroy_read_struct(&png_ptr, NULL, NULL);
        fclose(fp);
        return false;
    }

    if (setjmp(png_jmpbuf(png_ptr))) {
        ESP_LOGE(TAG, "PNG error during check");
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        fclose(fp);
        return false;
    }

    png_init_io(png_ptr, fp);
    png_set_sig_bytes(png_ptr, 8);
    png_read_info(png_ptr, info_ptr);

    int width = png_get_image_width(png_ptr, info_ptr);
    int height = png_get_image_height(png_ptr, info_ptr);

    if (width != BOARD_HAL_DISPLAY_WIDTH || height != BOARD_HAL_DISPLAY_HEIGHT) {
        ESP_LOGI(TAG, "Dimensions mismatch: %dx%d (expected %dx%d)", width, height,
                 BOARD_HAL_DISPLAY_WIDTH, BOARD_HAL_DISPLAY_HEIGHT);
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        fclose(fp);
        return false;
    }

    // Force RGB format
    png_set_expand(png_ptr);
    png_set_strip_alpha(png_ptr);
    png_set_packing(png_ptr);
    png_set_palette_to_rgb(png_ptr);
    png_read_update_info(png_ptr, info_ptr);

    if (png_get_channels(png_ptr, info_ptr) != 3) {
        ESP_LOGI(TAG, "Not RGB format");
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        fclose(fp);
        return false;
    }

    // Check pixels row by row
    png_bytep row = (png_bytep) malloc(png_get_rowbytes(png_ptr, info_ptr));
    if (!row) {
        ESP_LOGE(TAG, "Failed to allocate row buffer");
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        fclose(fp);
        return false;
    }

    bool valid = true;
    for (int y = 0; y < height; y++) {
        png_read_row(png_ptr, row, NULL);

        // Check every pixel in the row
        for (int x = 0; x < width; x++) {
            uint8_t r = row[x * 3];
            uint8_t g = row[x * 3 + 1];
            uint8_t b = row[x * 3 + 2];

            // Should be exactly one of the colors in the palette
            bool color_match = false;
            for (int i = 0; i < 7; i++) {
                if (i == 4)
                    continue;  // Skip reserved
                if (r == palette[i].r && g == palette[i].g && b == palette[i].b) {
                    color_match = true;
                    break;
                }
            }

            if (!color_match) {
                ESP_LOGI(TAG, "Pixel (%d,%d) color (%d,%d,%d) not in palette", x, y, r, g, b);
                valid = false;
                break;
            }
        }
        if (!valid)
            break;
    }

    free(row);
    png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
    fclose(fp);
    return valid;
}

bool image_processor_is_processed_buffer(const uint8_t *data, size_t size)
{
    if (!data || size < 8) {
        return false;
    }

    // Check PNG signature
    if (!(data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E && data[3] == 0x47 &&
          data[4] == 0x0D && data[5] == 0x0A && data[6] == 0x1A && data[7] == 0x0A)) {
        return false;
    }

    // Decode PNG from buffer to check dimensions and palette
    uint8_t *rgb_buffer = NULL;
    int width = 0, height = 0;

    if (decode_png_buffer(data, size, &rgb_buffer, &width, &height) != ESP_OK) {
        return false;
    }

    // Check dimensions
    if (width != BOARD_HAL_DISPLAY_WIDTH || height != BOARD_HAL_DISPLAY_HEIGHT) {
        ESP_LOGI(TAG, "Buffer dimensions mismatch: %dx%d (expected %dx%d)", width, height,
                 BOARD_HAL_DISPLAY_WIDTH, BOARD_HAL_DISPLAY_HEIGHT);
        heap_caps_free(rgb_buffer);
        return false;
    }

    // Check if all pixels are in palette
    bool valid = true;
    for (int i = 0; i < width * height && valid; i++) {
        uint8_t r = rgb_buffer[i * 3];
        uint8_t g = rgb_buffer[i * 3 + 1];
        uint8_t b = rgb_buffer[i * 3 + 2];

        bool color_match = false;
        for (int j = 0; j < 7; j++) {
            if (j == 4)
                continue;
            if (r == palette[j].r && g == palette[j].g && b == palette[j].b) {
                color_match = true;
                break;
            }
        }

        if (!color_match) {
            valid = false;
        }
    }

    heap_caps_free(rgb_buffer);
    return valid;
}

image_format_t image_processor_detect_format(const char *input_path)
{
    FILE *fp = fopen(input_path, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "Failed to open file for format detection: %s", input_path);
        return IMAGE_FORMAT_UNKNOWN;
    }

    uint8_t magic[8];
    size_t read = fread(magic, 1, 8, fp);
    fclose(fp);

    if (read < 2) {
        return IMAGE_FORMAT_UNKNOWN;
    }

    if (read >= 8 && magic[0] == 0x89 && magic[1] == 0x50 && magic[2] == 0x4E && magic[3] == 0x47 &&
        magic[4] == 0x0D && magic[5] == 0x0A && magic[6] == 0x1A && magic[7] == 0x0A) {
        return IMAGE_FORMAT_PNG;
    } else if (magic[0] == 0x42 && magic[1] == 0x4D) {
        return IMAGE_FORMAT_BMP;
    } else if (magic[0] == 0xFF && magic[1] == 0xD8) {
        return IMAGE_FORMAT_JPG;
    } else if (magic[0] == 0x1F && magic[1] == 0x8B) {
        return IMAGE_FORMAT_EPD_GZ;
    }

    return IMAGE_FORMAT_UNKNOWN;
}
