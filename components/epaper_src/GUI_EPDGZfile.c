// filename: GUI_EPDGZfile.c
#include "GUI_EPDGZfile.h"

#include <esp_log.h>
#include <stdlib.h>
#include <zlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "GUI_EPDGZfile";

// Compressed bytes read from the file per inflate step
#define EPDGZ_IN_CHUNK 2048

int GUI_ReadEPDGZRows(const char *path, size_t row_bytes, int height, GUI_EPDGZRowFn fn, void *ctx)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "Failed to open EPDGZ file: %s", path);
        return 1;
    }

    uint8_t *in = malloc(EPDGZ_IN_CHUNK);
    uint8_t *row = malloc(row_bytes);
    if (!in || !row) {
        ESP_LOGE(TAG, "Failed to allocate EPDGZ buffers");
        free(in);
        free(row);
        fclose(fp);
        return 1;
    }

    z_stream strm = {0};
    // 16 + MAX_WBITS enables gzip decoding
    if (inflateInit2(&strm, 16 + MAX_WBITS) != Z_OK) {
        ESP_LOGE(TAG, "inflateInit2 failed");
        free(in);
        free(row);
        fclose(fp);
        return 1;
    }

    int result = 1;
    int y = 0;
    int ret = Z_OK;
    strm.next_out = row;
    strm.avail_out = row_bytes;

    while (y < height) {
        if (strm.avail_in == 0) {
            size_t n = fread(in, 1, EPDGZ_IN_CHUNK, fp);
            if (n == 0) {
                ESP_LOGE(TAG, "EPDGZ ended after %d of %d rows", y, height);
                break;
            }
            strm.next_in = in;
            strm.avail_in = n;
        }

        ret = inflate(&strm, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END && ret != Z_BUF_ERROR) {
            ESP_LOGE(TAG, "Decompression failed: %d", ret);
            break;
        }

        if (strm.avail_out == 0) {
            if (fn(ctx, y, row) != 0) {
                break;
            }
            y++;
            strm.next_out = row;
            strm.avail_out = row_bytes;
            // Yield periodically so the IDLE task can feed the watchdog
            if ((y & 63) == 0) {
                vTaskDelay(1);
            }
        } else if (ret == Z_STREAM_END) {
            ESP_LOGE(TAG, "EPDGZ ended after %d of %d rows", y, height);
            break;
        }
    }

    // Decode on to the end of the gzip stream, so a truncated file or a bad
    // CRC still fails. Only the trailer may follow the last row: a single
    // byte of output space catches extra image data without inflating it all.
    while (y == height && ret != Z_STREAM_END) {
        if (strm.avail_in == 0) {
            size_t n = fread(in, 1, EPDGZ_IN_CHUNK, fp);
            if (n == 0) {
                ESP_LOGE(TAG, "EPDGZ is truncated before its gzip trailer");
                break;
            }
            strm.next_in = in;
            strm.avail_in = n;
        }
        strm.next_out = row;
        strm.avail_out = 1;
        ret = inflate(&strm, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END && ret != Z_BUF_ERROR) {
            ESP_LOGE(TAG, "Decompression failed: %d", ret);
            break;
        }
        if (strm.avail_out == 0) {
            ESP_LOGE(TAG, "EPDGZ holds more than %d rows", height);
            ret = Z_DATA_ERROR;
            break;
        }
    }

    if (y == height && ret == Z_STREAM_END) {
        result = 0;
    }

    inflateEnd(&strm);
    free(in);
    free(row);
    fclose(fp);
    return result;
}

static int paint_row(void *ctx, int y, const uint8_t *row)
{
    int width = Paint.Width;
    for (int x = 0; x < width; x += 2) {
        uint8_t byte = row[x / 2];
        Paint_SetPixel(x, y, (byte >> 4) & 0x0F);
        if (x + 1 < width) {
            Paint_SetPixel(x + 1, y, byte & 0x0F);
        }
    }
    return 0;
}

/**
 * @brief Read EPDGZ file and display it on the e-paper display
 *
 * Streams the gzip-compressed 4-bit-per-pixel raw e-paper image through
 * inflate and paints it to the display buffer using Paint_SetPixel.
 *
 * @param path Path to the EPDGZ file
 * @return 0 on success, non-zero on error
 */
int GUI_ReadEPDGZ(const char *path)
{
    int width = Paint.Width;
    int height = Paint.Height;

    if (GUI_ReadEPDGZRows(path, (width + 1) / 2, height, paint_row, NULL) != 0) {
        return 1;
    }

    ESP_LOGI(TAG, "EPDGZ displayed successfully (%dx%d)", width, height);
    return 0;
}
