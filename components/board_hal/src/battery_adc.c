#include "battery_adc.h"

#include <stdlib.h>

#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "battery_adc";

struct battery_adc {
    battery_adc_config_t cfg;
    adc_oneshot_unit_handle_t adc;
    adc_cali_handle_t cali;  // NULL if eFuse calibration is unavailable
};

esp_err_t battery_adc_create(const battery_adc_config_t *cfg, battery_adc_t **out)
{
    if (!cfg || !out)
        return ESP_ERR_INVALID_ARG;

    battery_adc_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return ESP_ERR_NO_MEM;
    ctx->cfg = *cfg;

    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = cfg->unit,
        .clk_src = ADC_DIGI_CLK_SRC_DEFAULT,
    };
    esp_err_t ret = adc_oneshot_new_unit(&init_config, &ctx->adc);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC init failed: %s", esp_err_to_name(ret));
        free(ctx);
        return ret;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = cfg->atten,
    };
    ret = adc_oneshot_config_channel(ctx->adc, cfg->channel, &chan_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC channel config failed: %s", esp_err_to_name(ret));
        adc_oneshot_del_unit(ctx->adc);
        free(ctx);
        return ret;
    }

    // eFuse-based calibration converts raw counts to accurate millivolts; the
    // ESP32-S3 ADC is nonlinear, so the raw*3300/4095 estimate drifts. If it is
    // unavailable, battery_adc_read_mv() falls back to that linear estimate.
    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = cfg->unit,
        .chan = cfg->channel,
        .atten = cfg->atten,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_config, &ctx->cali) != ESP_OK) {
        ESP_LOGW(TAG, "ADC calibration unavailable; using linear estimate");
        ctx->cali = NULL;
    }

    *out = ctx;
    return ESP_OK;
}

int battery_adc_read_mv(battery_adc_t *ctx)
{
    if (!ctx || !ctx->adc)
        return -1;
    const battery_adc_config_t *cfg = &ctx->cfg;

    if (cfg->enable_pin >= 0) {
        gpio_set_level((gpio_num_t) cfg->enable_pin, 1);
        if (cfg->settle_ms > 0)
            vTaskDelay(pdMS_TO_TICKS(cfg->settle_ms));
    }

    int samples = cfg->samples > 0 ? cfg->samples : 1;
    int64_t raw_sum = 0;
    int got = 0;
    for (int i = 0; i < samples; i++) {
        int raw;
        if (adc_oneshot_read(ctx->adc, cfg->channel, &raw) == ESP_OK) {
            raw_sum += raw;
            got++;
        }
    }

    if (cfg->enable_pin >= 0)
        gpio_set_level((gpio_num_t) cfg->enable_pin, 0);

    if (got == 0)
        return -1;
    int raw_avg = (int) (raw_sum / got);

    float scale = cfg->cal_scale > 0.0f ? cfg->cal_scale : 1.0f;
    int pin_mv;
    if (ctx->cali && adc_cali_raw_to_voltage(ctx->cali, raw_avg, &pin_mv) == ESP_OK) {
        // Calibrated mV at the ADC pin; undo the divider + per-unit correction.
        return (int) ((float) pin_mv * cfg->divider * scale);
    }
    // Fallback: uncalibrated linear estimate.
    return (int) ((float) raw_avg * (3300.0f / 4095.0f) * cfg->divider * scale);
}

void battery_adc_destroy(battery_adc_t *ctx)
{
    if (!ctx)
        return;
    if (ctx->cali)
        adc_cali_delete_scheme_curve_fitting(ctx->cali);
    if (ctx->adc)
        adc_oneshot_del_unit(ctx->adc);
    free(ctx);
}
