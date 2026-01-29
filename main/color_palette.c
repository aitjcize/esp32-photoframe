#include "color_palette.h"

#include <string.h>

#include "config.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "color_palette";

void color_palette_get_defaults(color_palette_t *palette)
{
    palette->black = (color_rgb_t){2, 2, 2};
    palette->white = (color_rgb_t){190, 190, 190};
    palette->yellow = (color_rgb_t){205, 202, 0};
    palette->red = (color_rgb_t){135, 19, 0};
    palette->blue = (color_rgb_t){5, 64, 158};
    palette->green = (color_rgb_t){39, 102, 60};
}

esp_err_t color_palette_init(void)
{
    ESP_LOGI(TAG, "Initializing color palette");
    return ESP_OK;
}

esp_err_t color_palette_save(const color_palette_t *palette)
{
    nvs_handle_t handle;
    esp_err_t err;

    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_u8(handle, "pal_black_r", palette->black.r);
    err |= nvs_set_u8(handle, "pal_black_g", palette->black.g);
    err |= nvs_set_u8(handle, "pal_black_b", palette->black.b);

    err |= nvs_set_u8(handle, "pal_white_r", palette->white.r);
    err |= nvs_set_u8(handle, "pal_white_g", palette->white.g);
    err |= nvs_set_u8(handle, "pal_white_b", palette->white.b);

    err |= nvs_set_u8(handle, "pal_yellow_r", palette->yellow.r);
    err |= nvs_set_u8(handle, "pal_yellow_g", palette->yellow.g);
    err |= nvs_set_u8(handle, "pal_yellow_b", palette->yellow.b);

    err |= nvs_set_u8(handle, "pal_red_r", palette->red.r);
    err |= nvs_set_u8(handle, "pal_red_g", palette->red.g);
    err |= nvs_set_u8(handle, "pal_red_b", palette->red.b);

    err |= nvs_set_u8(handle, "pal_blue_r", palette->blue.r);
    err |= nvs_set_u8(handle, "pal_blue_g", palette->blue.g);
    err |= nvs_set_u8(handle, "pal_blue_b", palette->blue.b);

    err |= nvs_set_u8(handle, "pal_green_r", palette->green.r);
    err |= nvs_set_u8(handle, "pal_green_g", palette->green.g);
    err |= nvs_set_u8(handle, "pal_green_b", palette->green.b);

    if (err == ESP_OK) {
        err = nvs_commit(handle);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Color palette saved to NVS");
        } else {
            ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGE(TAG, "Failed to write palette to NVS: %s", esp_err_to_name(err));
    }

    nvs_close(handle);
    return err;
}

esp_err_t color_palette_load(color_palette_t *palette)
{
    nvs_handle_t handle;
    esp_err_t err;

    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS for reading: %s, using defaults", esp_err_to_name(err));
        color_palette_get_defaults(palette);
        return err;
    }

    uint8_t val;
    bool has_palette = true;

    err = nvs_get_u8(handle, "pal_black_r", &val);
    if (err == ESP_OK)
        palette->black.r = val;
    else
        has_palette = false;
    err = nvs_get_u8(handle, "pal_black_g", &val);
    if (err == ESP_OK)
        palette->black.g = val;
    else
        has_palette = false;
    err = nvs_get_u8(handle, "pal_black_b", &val);
    if (err == ESP_OK)
        palette->black.b = val;
    else
        has_palette = false;

    err = nvs_get_u8(handle, "pal_white_r", &val);
    if (err == ESP_OK)
        palette->white.r = val;
    else
        has_palette = false;
    err = nvs_get_u8(handle, "pal_white_g", &val);
    if (err == ESP_OK)
        palette->white.g = val;
    else
        has_palette = false;
    err = nvs_get_u8(handle, "pal_white_b", &val);
    if (err == ESP_OK)
        palette->white.b = val;
    else
        has_palette = false;

    err = nvs_get_u8(handle, "pal_yellow_r", &val);
    if (err == ESP_OK)
        palette->yellow.r = val;
    else
        has_palette = false;
    err = nvs_get_u8(handle, "pal_yellow_g", &val);
    if (err == ESP_OK)
        palette->yellow.g = val;
    else
        has_palette = false;
    err = nvs_get_u8(handle, "pal_yellow_b", &val);
    if (err == ESP_OK)
        palette->yellow.b = val;
    else
        has_palette = false;

    err = nvs_get_u8(handle, "pal_red_r", &val);
    if (err == ESP_OK)
        palette->red.r = val;
    else
        has_palette = false;
    err = nvs_get_u8(handle, "pal_red_g", &val);
    if (err == ESP_OK)
        palette->red.g = val;
    else
        has_palette = false;
    err = nvs_get_u8(handle, "pal_red_b", &val);
    if (err == ESP_OK)
        palette->red.b = val;
    else
        has_palette = false;

    err = nvs_get_u8(handle, "pal_blue_r", &val);
    if (err == ESP_OK)
        palette->blue.r = val;
    else
        has_palette = false;
    err = nvs_get_u8(handle, "pal_blue_g", &val);
    if (err == ESP_OK)
        palette->blue.g = val;
    else
        has_palette = false;
    err = nvs_get_u8(handle, "pal_blue_b", &val);
    if (err == ESP_OK)
        palette->blue.b = val;
    else
        has_palette = false;

    err = nvs_get_u8(handle, "pal_green_r", &val);
    if (err == ESP_OK)
        palette->green.r = val;
    else
        has_palette = false;
    err = nvs_get_u8(handle, "pal_green_g", &val);
    if (err == ESP_OK)
        palette->green.g = val;
    else
        has_palette = false;
    err = nvs_get_u8(handle, "pal_green_b", &val);
    if (err == ESP_OK)
        palette->green.b = val;
    else
        has_palette = false;

    nvs_close(handle);

    if (!has_palette) {
        ESP_LOGW(TAG, "Color palette not found in NVS, using defaults");
        color_palette_get_defaults(palette);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Color palette loaded from NVS");
    return ESP_OK;
}
