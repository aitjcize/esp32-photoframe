#ifndef IMAGE_PROCESSOR_H
#define IMAGE_PROCESSOR_H

#include <stdbool.h>

#include "esp_err.h"

esp_err_t image_processor_init(void);
esp_err_t image_processor_convert_jpg_to_bmp(const char *jpg_path, const char *bmp_path,
                                             bool use_stock_mode, const char *dither_algorithm);
void image_processor_reload_palette(void);

#endif
