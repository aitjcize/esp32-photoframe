// filename: GUI_PNGfile.c
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <png.h>
#include <stdlib.h>

#include "GUI_Paint.h"

static const char *TAG = "GUI_PNGfile";

/**
 * @brief Read PNG file and display it on the e-paper display
 *
 * Reads a 24-bit RGB PNG file, converts it to 6-color palette, and paints
 * it directly to the display buffer using Paint_SetPixel.
 *
 * @param path Path to the PNG file
 * @param Xstart Starting X coordinate
 * @param Ystart Starting Y coordinate
 * @return 0 on success, 1 on error
 */
UBYTE GUI_ReadPng_RGB_6Color(const char *path, UWORD Xstart, UWORD Ystart)
{
    FILE *fp = NULL;
    png_structp png_ptr = NULL;
    png_infop info_ptr = NULL;
    png_bytep *volatile row_pointers = NULL;
    uint8_t *volatile rgb_buffer = NULL;

    ESP_LOGI(TAG, "Reading PNG: %s", path);

    // Open PNG file
    fp = fopen(path, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "Cannot open PNG file: %s", path);
        return 1;
    }

    // Verify PNG signature
    uint8_t sig[8];
    if (fread(sig, 1, 8, fp) != 8 || png_sig_cmp(sig, 0, 8) != 0) {
        ESP_LOGE(TAG, "Not a valid PNG file");
        fclose(fp);
        return 1;
    }

    // Create PNG read struct
    png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png_ptr) {
        ESP_LOGE(TAG, "Failed to create PNG read struct");
        fclose(fp);
        return 1;
    }

    // Create PNG info struct
    info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        ESP_LOGE(TAG, "Failed to create PNG info struct");
        png_destroy_read_struct(&png_ptr, NULL, NULL);
        fclose(fp);
        return 1;
    }

    // Set error handling
    if (setjmp(png_jmpbuf(png_ptr))) {
        ESP_LOGE(TAG, "PNG decode error");
        goto cleanup;
    }

    // Initialize PNG I/O
    png_init_io(png_ptr, fp);
    png_set_sig_bytes(png_ptr, 8);

    // Read PNG info
    png_read_info(png_ptr, info_ptr);

    int width = png_get_image_width(png_ptr, info_ptr);
    int height = png_get_image_height(png_ptr, info_ptr);
    png_byte color_type = png_get_color_type(png_ptr, info_ptr);
    png_byte bit_depth = png_get_bit_depth(png_ptr, info_ptr);

    ESP_LOGI(TAG, "PNG: %dx%d, color_type=%d, bit_depth=%d", width, height, color_type, bit_depth);

    // Convert to RGB888
    if (color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(png_ptr);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
        png_set_expand_gray_1_2_4_to_8(png_ptr);
    if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(png_ptr);
    if (bit_depth == 16)
        png_set_strip_16(png_ptr);
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png_ptr);
    if (color_type == PNG_COLOR_TYPE_RGB_ALPHA || color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_strip_alpha(png_ptr);

    png_read_update_info(png_ptr, info_ptr);

    // Get actual row bytes after transformations
    size_t rowbytes = png_get_rowbytes(png_ptr, info_ptr);

    // Allocate RGB buffer for one row
    size_t rgb_size = rowbytes;
    rgb_buffer = heap_caps_malloc(rgb_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rgb_buffer) {
        ESP_LOGE(TAG, "Failed to allocate RGB buffer (%zu bytes)", rgb_size);
        goto cleanup;
    }

    // Process image row by row
    ESP_LOGI(TAG, "PNG decoded successfully");

    for (int y = 0; y < height; y++) {
        png_read_row(png_ptr, (png_bytep) rgb_buffer, NULL);

        for (int x = 0; x < width; x++) {
            if (x >= Paint.Width || y >= Paint.Height) {
                continue;
            }

            int offset = x * 3;  // 3 bytes per pixel (RGB)
            uint8_t r = rgb_buffer[offset + 0];
            uint8_t g = rgb_buffer[offset + 1];
            uint8_t b = rgb_buffer[offset + 2];

            // Map RGB to 6-color palette index
            UBYTE color;
            if (r == 0 && g == 0 && b == 0) {
                color = 0;  // Black
            } else if (r == 255 && g == 255 && b == 255) {
                color = 1;  // White
            } else if (r == 255 && g == 255 && b == 0) {
                color = 2;  // Yellow
            } else if (r == 255 && g == 0 && b == 0) {
                color = 3;  // Red
            } else if (r == 0 && g == 0 && b == 255) {
                color = 5;  // Blue
            } else if (r == 0 && g == 255 && b == 0) {
                color = 6;  // Green
            } else {
                color = 1;  // Default to white for unknown colors
            }

            // Paint pixel directly (Paint rotation system handles coordinate transformations)
            Paint_SetPixel(Xstart + x, Ystart + y, color);
        }
    }

    heap_caps_free(rgb_buffer);
    png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
    fclose(fp);

    ESP_LOGI(TAG, "PNG displayed successfully");
    return 0;

cleanup:
    if (rgb_buffer)
        heap_caps_free(rgb_buffer);
    if (row_pointers)
        free(row_pointers);
    if (png_ptr)
        png_destroy_read_struct(&png_ptr, info_ptr ? &info_ptr : NULL, NULL);
    if (fp)
        fclose(fp);
    return 1;
}
