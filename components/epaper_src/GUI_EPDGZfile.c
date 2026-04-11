// filename: GUI_EPDGZfile.c
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <stdlib.h>
#include <zlib.h>

#include "GUI_Paint.h"

static const char *TAG = "GUI_EPDGZfile";

/**
 * @brief Read EPDGZ file and display it on the e-paper display
 *
 * Reads a gzip-compressed 4-bit-per-pixel raw e-paper image file,
 * decompresses it, and paints directly to the display buffer using
 * Paint_SetPixel.
 *
 * @param path Path to the EPDGZ file
 * @return 0 on success, non-zero on error
 */
int GUI_ReadEPDGZ(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "Failed to open EPDGZ file: %s", path);
        return 1;
    }

    fseek(fp, 0, SEEK_END);
    long compressed_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    uint8_t *compressed_data = heap_caps_malloc(compressed_size, MALLOC_CAP_SPIRAM);
    if (!compressed_data) {
        ESP_LOGE(TAG, "Failed to allocate memory for compressed data");
        fclose(fp);
        return 1;
    }
    fread(compressed_data, 1, compressed_size, fp);
    fclose(fp);

    int width = Paint.Width;
    int height = Paint.Height;
    int uncompressed_size = (width * height + 1) / 2;

    uint8_t *uncompressed_data = heap_caps_malloc(uncompressed_size, MALLOC_CAP_SPIRAM);
    if (!uncompressed_data) {
        ESP_LOGE(TAG, "Failed to allocate memory for decompressed data");
        heap_caps_free(compressed_data);
        return 1;
    }

    z_stream strm = {0};
    strm.avail_in = compressed_size;
    strm.next_in = compressed_data;
    strm.avail_out = uncompressed_size;
    strm.next_out = uncompressed_data;

    // 16 + MAX_WBITS enables gzip decoding
    if (inflateInit2(&strm, 16 + MAX_WBITS) != Z_OK) {
        ESP_LOGE(TAG, "inflateInit2 failed");
        heap_caps_free(compressed_data);
        heap_caps_free(uncompressed_data);
        return 1;
    }

    int ret = inflate(&strm, Z_FINISH);
    inflateEnd(&strm);
    heap_caps_free(compressed_data);

    if (ret != Z_STREAM_END && ret != Z_OK) {
        ESP_LOGE(TAG, "Decompression failed: %d", ret);
        heap_caps_free(uncompressed_data);
        return 1;
    }

    ESP_LOGI(TAG, "EPDGZ: %dx%d, compressed %ld -> %d bytes", width, height, compressed_size,
             uncompressed_size);

    int byteIdx = 0;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x += 2) {
            uint8_t byte = uncompressed_data[byteIdx++];
            uint8_t p1 = (byte >> 4) & 0x0F;
            uint8_t p2 = byte & 0x0F;
            Paint_SetPixel(x, y, p1);
            if (x + 1 < width) {
                Paint_SetPixel(x + 1, y, p2);
            }
        }
    }

    ESP_LOGI(TAG, "EPDGZ displayed successfully");
    heap_caps_free(uncompressed_data);
    return 0;
}
