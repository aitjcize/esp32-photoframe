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

#endif
