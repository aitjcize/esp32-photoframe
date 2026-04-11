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
} color_palette_t;

esp_err_t color_palette_init(void);
esp_err_t color_palette_save(const color_palette_t *palette);
esp_err_t color_palette_load(color_palette_t *palette);
void color_palette_get_defaults(color_palette_t *palette);
char *color_palette_to_json(const color_palette_t *palette);
void color_palette_from_json(cJSON *json, color_palette_t *palette);

#endif
