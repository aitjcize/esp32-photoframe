#include "color_palette.h"

#include <string.h>

#include "cJSON.h"
#include "config.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "color_palette";
static const char *NVS_KEY_PALETTE = "palette";

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

    err = nvs_set_blob(handle, NVS_KEY_PALETTE, palette, sizeof(color_palette_t));
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

    // Initialize with defaults first
    color_palette_get_defaults(palette);

    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS for reading: %s, using defaults", esp_err_to_name(err));
        return ESP_OK;
    }

    size_t required_size = sizeof(color_palette_t);
    err = nvs_get_blob(handle, NVS_KEY_PALETTE, palette, &required_size);

    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Color palette not found in NVS, using defaults");
        color_palette_get_defaults(palette);
    } else {
        ESP_LOGI(TAG, "Color palette loaded from NVS");
    }

    return ESP_OK;
}

static cJSON *color_to_json(const color_rgb_t *color)
{
    cJSON *obj = cJSON_CreateObject();
    if (obj) {
        cJSON_AddNumberToObject(obj, "r", color->r);
        cJSON_AddNumberToObject(obj, "g", color->g);
        cJSON_AddNumberToObject(obj, "b", color->b);
    }
    return obj;
}

char *color_palette_to_json(const color_palette_t *palette)
{
    cJSON *json = cJSON_CreateObject();
    if (!json) {
        return NULL;
    }

    cJSON_AddItemToObject(json, "black", color_to_json(&palette->black));
    cJSON_AddItemToObject(json, "white", color_to_json(&palette->white));
    cJSON_AddItemToObject(json, "yellow", color_to_json(&palette->yellow));
    cJSON_AddItemToObject(json, "red", color_to_json(&palette->red));
    cJSON_AddItemToObject(json, "blue", color_to_json(&palette->blue));
    cJSON_AddItemToObject(json, "green", color_to_json(&palette->green));

    char *json_str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);

    return json_str;
}
