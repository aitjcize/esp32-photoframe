// Host-test stub for esp_jpeg's jpeg_decoder.h — JPEG decoding is not
// exercised on host; the stubs report failure so PNG paths stay testable.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    JPEG_IMAGE_FORMAT_RGB888,
    JPEG_IMAGE_FORMAT_RGB565,
} esp_jpeg_image_format_t;

typedef enum {
    JPEG_IMAGE_SCALE_0 = 0,
    JPEG_IMAGE_SCALE_1_2 = 1,
    JPEG_IMAGE_SCALE_1_4 = 2,
    JPEG_IMAGE_SCALE_1_8 = 3,
} esp_jpeg_image_scale_t;

typedef struct {
    uint8_t *indata;
    size_t indata_size;
    uint8_t *outbuf;
    size_t outbuf_size;
    esp_jpeg_image_format_t out_format;
    esp_jpeg_image_scale_t out_scale;
    struct {
        uint8_t swap_color_bytes : 1;
    } flags;
} esp_jpeg_image_cfg_t;

typedef struct {
    int width;
    int height;
    size_t output_len;
} esp_jpeg_image_output_t;

esp_err_t esp_jpeg_get_image_info(esp_jpeg_image_cfg_t *cfg, esp_jpeg_image_output_t *img);
esp_err_t esp_jpeg_decode(esp_jpeg_image_cfg_t *cfg, esp_jpeg_image_output_t *img);

#ifdef __cplusplus
}
#endif
