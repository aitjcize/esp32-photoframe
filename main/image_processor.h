#ifndef IMAGE_PROCESSOR_H
#define IMAGE_PROCESSOR_H

#include <stdbool.h>

#include "esp_err.h"

typedef enum {
    DITHER_FLOYD_STEINBERG,
    DITHER_STUCKI,
    DITHER_BURKES,
    DITHER_SIERRA
} dither_algorithm_t;

esp_err_t image_processor_init(void);

esp_err_t image_processor_process(const char *input_path, const char *output_path,
                                  bool use_stock_mode, dither_algorithm_t dither_algorithm);

esp_err_t image_processor_reload_palette(void);

bool image_processor_is_processed(const char *input_path);

typedef enum {
    IMAGE_FORMAT_UNKNOWN,
    IMAGE_FORMAT_PNG,
    IMAGE_FORMAT_BMP,
    IMAGE_FORMAT_JPG
} image_format_t;

image_format_t image_processor_detect_format(const char *input_path);

#endif
