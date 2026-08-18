// Host-test stub implementations for the ESP-IDF symbols image_processor.c
// links against.
#include <stddef.h>

#include "board_hal.h"
#include "color_palette.h"
#include "esp_err.h"
#include "jpeg_decoder.h"

int test_board_display_width = 800;
int test_board_display_height = 480;
const char *test_board_display_type = "spectra6";

const char *esp_err_to_name(esp_err_t code)
{
    return code == ESP_OK ? "ESP_OK" : "ESP_ERR";
}

// color_palette normally loads calibration from NVS; on host, always report
// the firmware defaults (mirrors color_palette_get_defaults, whose real
// implementation drags in NVS).
esp_err_t color_palette_load(color_palette_t *palette)
{
    palette->black = (color_rgb_t){2, 2, 2};
    palette->white = (color_rgb_t){190, 200, 200};
    palette->yellow = (color_rgb_t){205, 202, 0};
    palette->red = (color_rgb_t){135, 19, 0};
    palette->blue = (color_rgb_t){5, 64, 158};
    palette->green = (color_rgb_t){39, 102, 60};
    palette->gray_black_y = 0.009f;
    palette->gray_white_y = 0.65f;
    palette->gray_gamma = 1.42f;
    return ESP_OK;
}

esp_err_t esp_jpeg_get_image_info(esp_jpeg_image_cfg_t *cfg, esp_jpeg_image_output_t *img)
{
    (void) cfg;
    img->width = 0;
    img->height = 0;
    img->output_len = 0;
    return ESP_FAIL;
}

esp_err_t esp_jpeg_decode(esp_jpeg_image_cfg_t *cfg, esp_jpeg_image_output_t *img)
{
    (void) cfg;
    (void) img;
    return ESP_FAIL;
}
