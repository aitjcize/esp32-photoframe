#ifndef COLOR_PALETTE_H
#define COLOR_PALETTE_H

#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} color_rgb_t;

typedef struct {
    color_rgb_t black;
    color_rgb_t white;
    color_rgb_t yellow;
    color_rgb_t red;
    color_rgb_t blue;
    color_rgb_t green;
    // GC16 grayscale calibration: measured RELATIVE LUMINANCE (Y, 0..1) of full
    // black / full white, plus a mid-level gamma (1.0 = perceptually linear,
    // >1 darkens mids). The 16-level perceived ramp is derived from these
    // downstream (epaper-image-convert). Unused for color panels.
    float gray_black_y;
    float gray_white_y;
    float gray_gamma;
} color_palette_t;

esp_err_t color_palette_init(void);
esp_err_t color_palette_save(const color_palette_t *palette);
esp_err_t color_palette_load(color_palette_t *palette);
void color_palette_get_defaults(color_palette_t *palette);
char *color_palette_to_json(const color_palette_t *palette);
void color_palette_from_json(cJSON *json, color_palette_t *palette);

#endif
