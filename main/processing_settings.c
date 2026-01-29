#include "processing_settings.h"

#include <string.h>

#include "cJSON.h"
#include "config.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "processing_settings";

#define NVS_PROC_EXPOSURE_KEY "proc_exp"
#define NVS_PROC_SATURATION_KEY "proc_sat"
#define NVS_PROC_TONE_MODE_KEY "proc_tone"
#define NVS_PROC_CONTRAST_KEY "proc_cont"
#define NVS_PROC_STRENGTH_KEY "proc_str"
#define NVS_PROC_SHADOW_KEY "proc_shad"
#define NVS_PROC_HIGHLIGHT_KEY "proc_high"
#define NVS_PROC_MIDPOINT_KEY "proc_mid"
#define NVS_PROC_COLOR_METHOD_KEY "proc_col"
#define NVS_PROC_MODE_KEY "proc_mode"
#define NVS_PROC_COMPRESS_DR_KEY "proc_cdr"
#define NVS_PROC_DITHER_ALGO_KEY "proc_dith"

void processing_settings_get_defaults(processing_settings_t *settings)
{
    settings->exposure = 1.0f;
    settings->saturation = 1.0f;
    strncpy(settings->tone_mode, "contrast", sizeof(settings->tone_mode) - 1);
    settings->contrast = 1.0f;
    settings->strength = 0.5f;
    settings->shadow_boost = 0.0f;
    settings->highlight_compress = 0.0f;
    settings->midpoint = 0.5f;
    strncpy(settings->color_method, "rgb", sizeof(settings->color_method) - 1);
    strncpy(settings->processing_mode, "enhanced", sizeof(settings->processing_mode) - 1);
    strncpy(settings->dither_algorithm, "floyd-steinberg", sizeof(settings->dither_algorithm) - 1);
    settings->compress_dynamic_range = true;
}

dither_algorithm_t processing_settings_get_dithering_algorithm(void)
{
    processing_settings_t settings;
    if (processing_settings_load(&settings) != ESP_OK) {
        processing_settings_get_defaults(&settings);
    }

    // Parse dithering algorithm string to enum
    if (strcmp(settings.dither_algorithm, "stucki") == 0) {
        return DITHER_STUCKI;
    } else if (strcmp(settings.dither_algorithm, "burkes") == 0) {
        return DITHER_BURKES;
    } else if (strcmp(settings.dither_algorithm, "sierra") == 0) {
        return DITHER_SIERRA;
    }

    return DITHER_FLOYD_STEINBERG;  // default
}

esp_err_t processing_settings_init(void)
{
    ESP_LOGI(TAG, "Processing settings initialized");
    return ESP_OK;
}

esp_err_t processing_settings_save(const processing_settings_t *settings)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing: %s", esp_err_to_name(err));
        return err;
    }

    // Save float values as uint32_t to avoid precision issues
    uint32_t exp_bits, sat_bits, cont_bits, str_bits, shad_bits, high_bits, mid_bits;
    memcpy(&exp_bits, &settings->exposure, sizeof(float));
    memcpy(&sat_bits, &settings->saturation, sizeof(float));
    memcpy(&cont_bits, &settings->contrast, sizeof(float));
    memcpy(&str_bits, &settings->strength, sizeof(float));
    memcpy(&shad_bits, &settings->shadow_boost, sizeof(float));
    memcpy(&high_bits, &settings->highlight_compress, sizeof(float));
    memcpy(&mid_bits, &settings->midpoint, sizeof(float));

    nvs_set_u32(nvs_handle, NVS_PROC_EXPOSURE_KEY, exp_bits);
    nvs_set_u32(nvs_handle, NVS_PROC_SATURATION_KEY, sat_bits);
    nvs_set_str(nvs_handle, NVS_PROC_TONE_MODE_KEY, settings->tone_mode);
    nvs_set_u32(nvs_handle, NVS_PROC_CONTRAST_KEY, cont_bits);
    nvs_set_u32(nvs_handle, NVS_PROC_STRENGTH_KEY, str_bits);
    nvs_set_u32(nvs_handle, NVS_PROC_SHADOW_KEY, shad_bits);
    nvs_set_u32(nvs_handle, NVS_PROC_HIGHLIGHT_KEY, high_bits);
    nvs_set_u32(nvs_handle, NVS_PROC_MIDPOINT_KEY, mid_bits);
    nvs_set_str(nvs_handle, NVS_PROC_COLOR_METHOD_KEY, settings->color_method);
    nvs_set_str(nvs_handle, NVS_PROC_MODE_KEY, settings->processing_mode);
    nvs_set_u8(nvs_handle, NVS_PROC_COMPRESS_DR_KEY, settings->compress_dynamic_range ? 1 : 0);
    nvs_set_str(nvs_handle, NVS_PROC_DITHER_ALGO_KEY, settings->dither_algorithm);

    err = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Processing settings saved to NVS");
    } else {
        ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(err));
    }

    return err;
}

esp_err_t processing_settings_load(processing_settings_t *settings)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS for reading, using defaults: %s", esp_err_to_name(err));
        processing_settings_get_defaults(settings);
        return err;
    }

    // Load with defaults as fallback
    processing_settings_get_defaults(settings);

    uint32_t exp_bits, sat_bits, cont_bits, str_bits, shad_bits, high_bits, mid_bits;

    if (nvs_get_u32(nvs_handle, NVS_PROC_EXPOSURE_KEY, &exp_bits) == ESP_OK) {
        memcpy(&settings->exposure, &exp_bits, sizeof(float));
    }
    if (nvs_get_u32(nvs_handle, NVS_PROC_SATURATION_KEY, &sat_bits) == ESP_OK) {
        memcpy(&settings->saturation, &sat_bits, sizeof(float));
    }

    size_t len = sizeof(settings->tone_mode);
    nvs_get_str(nvs_handle, NVS_PROC_TONE_MODE_KEY, settings->tone_mode, &len);

    if (nvs_get_u32(nvs_handle, NVS_PROC_CONTRAST_KEY, &cont_bits) == ESP_OK) {
        memcpy(&settings->contrast, &cont_bits, sizeof(float));
    }
    if (nvs_get_u32(nvs_handle, NVS_PROC_STRENGTH_KEY, &str_bits) == ESP_OK) {
        memcpy(&settings->strength, &str_bits, sizeof(float));
    }
    if (nvs_get_u32(nvs_handle, NVS_PROC_SHADOW_KEY, &shad_bits) == ESP_OK) {
        memcpy(&settings->shadow_boost, &shad_bits, sizeof(float));
    }
    if (nvs_get_u32(nvs_handle, NVS_PROC_HIGHLIGHT_KEY, &high_bits) == ESP_OK) {
        memcpy(&settings->highlight_compress, &high_bits, sizeof(float));
    }
    if (nvs_get_u32(nvs_handle, NVS_PROC_MIDPOINT_KEY, &mid_bits) == ESP_OK) {
        memcpy(&settings->midpoint, &mid_bits, sizeof(float));
    }

    len = sizeof(settings->color_method);
    nvs_get_str(nvs_handle, NVS_PROC_COLOR_METHOD_KEY, settings->color_method, &len);

    len = sizeof(settings->processing_mode);
    nvs_get_str(nvs_handle, NVS_PROC_MODE_KEY, settings->processing_mode, &len);

    uint8_t compress_dr = 0;
    if (nvs_get_u8(nvs_handle, NVS_PROC_COMPRESS_DR_KEY, &compress_dr) == ESP_OK) {
        settings->compress_dynamic_range = (compress_dr != 0);
    }

    len = sizeof(settings->dither_algorithm);
    nvs_get_str(nvs_handle, NVS_PROC_DITHER_ALGO_KEY, settings->dither_algorithm, &len);

    nvs_close(nvs_handle);

    return ESP_OK;
}

char *processing_settings_to_json(const processing_settings_t *settings)
{
    cJSON *json = cJSON_CreateObject();
    if (!json) {
        return NULL;
    }

    cJSON_AddNumberToObject(json, "exposure", settings->exposure);
    cJSON_AddNumberToObject(json, "saturation", settings->saturation);
    cJSON_AddStringToObject(json, "toneMode", settings->tone_mode);
    cJSON_AddNumberToObject(json, "contrast", settings->contrast);
    cJSON_AddNumberToObject(json, "strength", settings->strength);
    cJSON_AddNumberToObject(json, "shadowBoost", settings->shadow_boost);
    cJSON_AddNumberToObject(json, "highlightCompress", settings->highlight_compress);
    cJSON_AddNumberToObject(json, "midpoint", settings->midpoint);
    cJSON_AddStringToObject(json, "colorMethod", settings->color_method);
    cJSON_AddStringToObject(json, "processingMode", settings->processing_mode);
    cJSON_AddStringToObject(json, "ditherAlgorithm", settings->dither_algorithm);
    cJSON_AddBoolToObject(json, "compressDynamicRange", settings->compress_dynamic_range);

    char *json_str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);

    return json_str;
}
