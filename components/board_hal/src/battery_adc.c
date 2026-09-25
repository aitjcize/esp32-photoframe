#include "battery_adc.h"

#include <stdlib.h>

#include "board_hal.h"
#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "battery_adc";
static bool use_boot_sample;

void board_hal_use_boot_battery_sample(bool enabled)
{
    use_boot_sample = enabled;
}

static int battery_adc_sample_mv(battery_adc_t *ctx);

struct battery_adc {
    battery_adc_config_t cfg;
    int boot_mv;
    adc_oneshot_unit_handle_t adc;
    adc_cali_handle_t cali;  // NULL if eFuse calibration is unavailable
};

// Reference voltage used when an ESP32 has no Two-Point eFuse burned and the
// line-fitting scheme falls back to a default Vref. 1100 mV is the ESP32's
// nominal ADC reference and what esp_adc_cal used before the new API.
#define BATTERY_ADC_DEFAULT_VREF_MV 1100

// Create the calibration scheme the target actually supports: curve fitting on
// the ESP32-S3 (and friends), line fitting on the original ESP32. Returns
// ESP_ERR_NOT_SUPPORTED if neither is compiled in.
static esp_err_t battery_adc_cali_create(const battery_adc_config_t *cfg, adc_cali_handle_t *out)
{
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = cfg->unit,
        .chan = cfg->channel,
        .atten = cfg->atten,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    return adc_cali_create_scheme_curve_fitting(&cali_config, out);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cali_config = {
        .unit_id = cfg->unit,
        .atten = cfg->atten,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .default_vref = BATTERY_ADC_DEFAULT_VREF_MV,
    };
    return adc_cali_create_scheme_line_fitting(&cali_config, out);
#else
    (void) cfg;
    (void) out;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static void battery_adc_cali_destroy(adc_cali_handle_t cali)
{
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_delete_scheme_curve_fitting(cali);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_delete_scheme_line_fitting(cali);
#else
    (void) cali;
#endif
}

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
    if (battery_adc_cali_create(cfg, &ctx->cali) != ESP_OK) {
        ESP_LOGW(TAG, "ADC calibration unavailable; using linear estimate");
        ctx->cali = NULL;
    }

    if (cfg->sample_at_create) {
        ctx->boot_mv = battery_adc_sample_mv(ctx);
        ESP_LOGI(TAG, "Pre-display battery sample: %d mV", ctx->boot_mv);
    }

    *out = ctx;
    return ESP_OK;
}

int battery_adc_read_mv(battery_adc_t *ctx)
{
    if (!ctx)
        return -1;
    if (use_boot_sample && ctx->cfg.sample_at_create)
        return ctx->boot_mv;
    return battery_adc_sample_mv(ctx);
}

static int battery_adc_sample_mv(battery_adc_t *ctx)
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
        battery_adc_cali_destroy(ctx->cali);
    if (ctx->adc)
        adc_oneshot_del_unit(ctx->adc);
    free(ctx);
}
