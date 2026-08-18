#ifndef PROCESSING_SETTINGS_H
#define PROCESSING_SETTINGS_H

#include <stdbool.h>

#include "cJSON.h"
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
    char dither_algorithm[20];  // "floyd-steinberg", "stucki", "burkes", "sierra"
    bool compress_dynamic_range;
    char scale_mode[8];         // "cover" (crop to fill) or "fit" (letterbox)
    char background_color[12];  // palette color name for fit-mode letterbox bars
} processing_settings_t;

typedef enum {
    SCALE_MODE_COVER = 0,
    SCALE_MODE_FIT = 1,
} scale_mode_t;

esp_err_t processing_settings_init(void);
esp_err_t processing_settings_save(const processing_settings_t *settings);
esp_err_t processing_settings_load(processing_settings_t *settings);
void processing_settings_get_defaults(processing_settings_t *settings);
dither_algorithm_t processing_settings_get_dithering_algorithm(void);
scale_mode_t processing_settings_get_scale_mode(void);
// Palette color name for the fit-mode letterbox background ("white", ...)
void processing_settings_get_background_color(char *out, size_t out_size);
char *processing_settings_to_json(const processing_settings_t *settings);
void processing_settings_from_json(cJSON *json, processing_settings_t *settings);

#endif
