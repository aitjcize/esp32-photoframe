#include "png_decoder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "board_hal.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "png.h"

static const char *TAG = "png_decoder";

// Write BMP file header
static void write_bmp_header(FILE *fp, int width, int height)
{
    const int row_size = ((width * 3 + 3) / 4) * 4;  // Row size padded to 4 bytes
    const int image_size = row_size * height;
    const int file_size = 54 + image_size;

    uint8_t header[54] = {0};

    // BMP file header (14 bytes)
    header[0] = 'B';
    header[1] = 'M';
    header[2] = file_size & 0xFF;
    header[3] = (file_size >> 8) & 0xFF;
    header[4] = (file_size >> 16) & 0xFF;
    header[5] = (file_size >> 24) & 0xFF;
    header[10] = 54;  // Offset to pixel data

    // DIB header (40 bytes)
    header[14] = 40;  // DIB header size
    header[18] = width & 0xFF;
    header[19] = (width >> 8) & 0xFF;
    header[20] = (width >> 16) & 0xFF;
    header[21] = (width >> 24) & 0xFF;
    header[22] = height & 0xFF;
    header[23] = (height >> 8) & 0xFF;
    header[24] = (height >> 16) & 0xFF;
    header[25] = (height >> 24) & 0xFF;
    header[26] = 1;   // Planes
    header[28] = 24;  // Bits per pixel
    header[34] = image_size & 0xFF;
    header[35] = (image_size >> 8) & 0xFF;
    header[36] = (image_size >> 16) & 0xFF;
    header[37] = (image_size >> 24) & 0xFF;
    header[38] = 0x13;  // X pixels per meter (2835)
    header[39] = 0x0B;
    header[42] = 0x13;  // Y pixels per meter (2835)
    header[43] = 0x0B;

    fwrite(header, 1, 54, fp);
}

esp_err_t png_decode_to_bmp(const char *png_path, const char *bmp_path)
{
    volatile esp_err_t ret = ESP_FAIL;
    FILE *png_fp = NULL;
    FILE *volatile bmp_fp = NULL;
    png_structp png_ptr = NULL;
    png_infop info_ptr = NULL;
    uint8_t *volatile rgb_buffer = NULL;
    png_bytep *volatile row_pointers = NULL;

    ESP_LOGI(TAG, "Decoding PNG: %s -> %s", png_path, bmp_path);
    int64_t start_time = esp_timer_get_time();

    // Open PNG file
    png_fp = fopen(png_path, "rb");
    if (!png_fp) {
        ESP_LOGE(TAG, "Failed to open PNG file: %s", png_path);
        return ESP_FAIL;
    }

    // Verify PNG signature
    uint8_t sig[8];
    if (fread(sig, 1, 8, png_fp) != 8 || png_sig_cmp(sig, 0, 8) != 0) {
        ESP_LOGE(TAG, "Not a valid PNG file");
        fclose(png_fp);
        return ESP_FAIL;
    }

    // Create PNG read struct
    png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png_ptr) {
        ESP_LOGE(TAG, "Failed to create PNG read struct");
        fclose(png_fp);
        return ESP_FAIL;
    }

    // Create PNG info struct
    info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        ESP_LOGE(TAG, "Failed to create PNG info struct");
        png_destroy_read_struct(&png_ptr, NULL, NULL);
        fclose(png_fp);
        return ESP_FAIL;
    }

    // Set error handling
    if (setjmp(png_jmpbuf(png_ptr))) {
        ESP_LOGE(TAG, "PNG decode error");
        goto cleanup;
    }

    // Initialize PNG I/O
    png_init_io(png_ptr, png_fp);
    png_set_sig_bytes(png_ptr, 8);

    // Read PNG info
    png_read_info(png_ptr, info_ptr);

    int width = png_get_image_width(png_ptr, info_ptr);
    int height = png_get_image_height(png_ptr, info_ptr);
    png_byte color_type = png_get_color_type(png_ptr, info_ptr);
    png_byte bit_depth = png_get_bit_depth(png_ptr, info_ptr);

    ESP_LOGI(TAG, "PNG: %dx%d, color_type=%d, bit_depth=%d", width, height, color_type, bit_depth);

    // Validate resolution
    if (width != BOARD_HAL_DISPLAY_WIDTH || height != BOARD_HAL_DISPLAY_HEIGHT) {
        ESP_LOGE(TAG, "Invalid PNG resolution: %dx%d (expected %dx%d)", width, height,
                 BOARD_HAL_DISPLAY_WIDTH, BOARD_HAL_DISPLAY_HEIGHT);
        ret = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

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

    // Allocate RGB buffer
    size_t rgb_size = rowbytes * height;
    rgb_buffer = heap_caps_malloc(rgb_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rgb_buffer) {
        ESP_LOGE(TAG, "Failed to allocate RGB buffer (%zu bytes)", rgb_size);
        goto cleanup;
    }

    // Allocate row pointers
    row_pointers = malloc(sizeof(png_bytep) * height);
    if (!row_pointers) {
        ESP_LOGE(TAG, "Failed to allocate row pointers");
        goto cleanup;
    }

    for (int y = 0; y < height; y++) {
        row_pointers[y] = (png_bytep) (rgb_buffer + (y * rowbytes));
    }

    // Read image data
    png_read_image(png_ptr, (png_bytepp) row_pointers);

    int64_t decode_time = esp_timer_get_time();
    ESP_LOGI(TAG, "PNG decoded successfully in %.2f ms", (decode_time - start_time) / 1000.0);

    // Benchmark: Test PNG write speed by copying the original PNG file
    int64_t png_write_start = esp_timer_get_time();
    FILE *png_read_fp = fopen(png_path, "rb");
    if (png_read_fp) {
        // Get PNG file size
        fseek(png_read_fp, 0, SEEK_END);
        long png_size = ftell(png_read_fp);
        fseek(png_read_fp, 0, SEEK_SET);

        // Read entire PNG into buffer
        uint8_t *png_buffer = malloc(png_size);
        if (png_buffer) {
            fread(png_buffer, 1, png_size, png_read_fp);
            fclose(png_read_fp);

            // Write PNG to test file
            char test_png_path[256];
            snprintf(test_png_path, sizeof(test_png_path), "%s.benchmark", png_path);
            FILE *png_write_fp = fopen(test_png_path, "wb");
            if (png_write_fp) {
                static char png_write_buffer[64 * 1024];
                setvbuf(png_write_fp, png_write_buffer, _IOFBF, sizeof(png_write_buffer));
                fwrite(png_buffer, 1, png_size, png_write_fp);
                fclose(png_write_fp);
                unlink(test_png_path);  // Clean up test file

                int64_t png_write_end = esp_timer_get_time();
                ESP_LOGI(TAG, "Benchmark: PNG write (%ld bytes) took %.2f ms (%.2f KB/s)", png_size,
                         (png_write_end - png_write_start) / 1000.0,
                         (png_size / 1024.0) / ((png_write_end - png_write_start) / 1000000.0));
            }
            free(png_buffer);
        } else {
            fclose(png_read_fp);
        }
    }

    // Write BMP file
    bmp_fp = fopen(bmp_path, "wb");
    if (!bmp_fp) {
        ESP_LOGE(TAG, "Failed to create BMP file: %s", bmp_path);
        goto cleanup;
    }

    // Set large buffer for SD card writes (improves performance significantly)
    static char write_buffer[64 * 1024];  // 64KB buffer
    setvbuf((FILE *) bmp_fp, write_buffer, _IOFBF, sizeof(write_buffer));

    write_bmp_header((FILE *) bmp_fp, width, height);

    // Write pixel data (bottom-up, BGR format)
    // Allocate buffer for entire BMP image to write in one operation (much faster for SD card)
    const int bmp_row_size = ((width * 3 + 3) / 4) * 4;
    const size_t bmp_data_size = bmp_row_size * height;
    uint8_t *bmp_buffer = heap_caps_malloc(bmp_data_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!bmp_buffer) {
        ESP_LOGE(TAG, "Failed to allocate BMP buffer (%zu bytes)", bmp_data_size);
        goto cleanup;
    }

    // Convert RGB to BGR and flip vertically (BMP is bottom-up)
    for (int y = 0; y < height; y++) {
        int bmp_y = height - 1 - y;  // Flip vertically
        uint8_t *bmp_row = bmp_buffer + (bmp_y * bmp_row_size);
        memset(bmp_row, 0, bmp_row_size);  // Clear padding

        for (int x = 0; x < width; x++) {
            int src_offset = (y * rowbytes) + (x * 3);
            int dst_offset = x * 3;
            // Convert RGB to BGR
            bmp_row[dst_offset + 0] = ((uint8_t *) rgb_buffer)[src_offset + 2];  // B
            bmp_row[dst_offset + 1] = ((uint8_t *) rgb_buffer)[src_offset + 1];  // G
            bmp_row[dst_offset + 2] = ((uint8_t *) rgb_buffer)[src_offset + 0];  // R
        }
    }

    // Write entire BMP in one operation (much faster than row-by-row)
    size_t written = fwrite(bmp_buffer, 1, bmp_data_size, (FILE *) bmp_fp);
    free(bmp_buffer);

    if (written != bmp_data_size) {
        ESP_LOGE(TAG, "Failed to write BMP data: wrote %zu of %zu bytes", written, bmp_data_size);
        goto cleanup;
    }

    int64_t end_time = esp_timer_get_time();
    ESP_LOGI(TAG, "BMP file written successfully");
    ESP_LOGI(TAG, "Total PNG->BMP conversion time: %.2f ms", (end_time - start_time) / 1000.0);
    ret = ESP_OK;

cleanup:
    if (row_pointers)
        free((void *) row_pointers);
    if (rgb_buffer)
        free((void *) rgb_buffer);
    if (png_ptr)
        png_destroy_read_struct(&png_ptr, info_ptr ? &info_ptr : NULL, NULL);
    if (png_fp)
        fclose(png_fp);
    if (bmp_fp)
        fclose((FILE *) bmp_fp);

    return ret;
}
