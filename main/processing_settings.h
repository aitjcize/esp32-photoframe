#ifndef PROCESSING_SETTINGS_H
#define PROCESSING_SETTINGS_H

#include <stdbool.h>

#include "esp_err.h"
#include "image_processor.h"

typedef struct {
    float exposure;
    float saturation;
    char tone_mode[16];  // "scurve" or "contrast"
    float contrast;
    float strength;
    float shadow_boost;
    float highlight_compress;
    float midpoint;
    char color_method[8];       // "rgb" or "lab"
    char processing_mode[16];   // "enhanced" or "stock"
    char dither_algorithm[20];  // "floyd-steinberg", "stucki", "burkes", "sierra"
    bool compress_dynamic_range;
} processing_settings_t;

esp_err_t processing_settings_init(void);
esp_err_t processing_settings_save(const processing_settings_t *settings);
esp_err_t processing_settings_load(processing_settings_t *settings);
void processing_settings_get_defaults(processing_settings_t *settings);
dither_algorithm_t processing_settings_get_dithering_algorithm(void);
char *processing_settings_to_json(const processing_settings_t *settings);

#endif
