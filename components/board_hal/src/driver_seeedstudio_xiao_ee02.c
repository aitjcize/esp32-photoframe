#include <esp_timer.h>

#include "board_hal.h"
#include "driver/gpio.h"
#include "epaper_port.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "soc/soc_caps.h"

static const char *TAG = "board_hal_ee02";

// Pin Definitions for XIAO EE02
// Based on typical XIAO ESP32-S3 usage:
// VBAT read often uses GPIO 1 with divider, or internal measurement.
// XIAO ESP32-S3 has precise battery voltage on IO1 (A0) via voltage divider (R11=100k, R10=100k =>
// factor 2) D0/GPIO1 is A0
#define VBAT_ADC_CHANNEL ADC_CHANNEL_0  // GPIO 1 is ADC1_CHANNEL_0
#define VBAT_VOLTAGE_DIVIDER 2.0f

// USB detection (VBUS)
// Standard XIAO S3 detects VBUS via GPIO? Or rely on internal USB serial?
// For now, simpler approach: assume USB connected if we can print to log?
// Actually, BQ24070 has PGOOD / CHG outputs but they might not be wired to GPIOs on the shield.
// Let's rely on empirical behavior or assume always connected for now if not defined.
// NOTE: EE02 schematic shows:
// BAT_ADC -> GPIO 1 (D0/A0)
// No direct VBUS GPIO shown on simple schematic, often internal or not wired.

static adc_oneshot_unit_handle_t adc_handle = NULL;

esp_err_t board_hal_init(void)
{
    ESP_LOGI(TAG, "Initializing XIAO EE02 Power HAL (BQ24070)");

    // Do nothing for now.
    return ESP_OK;

    // Initialize ADC for battery voltage
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT_1,
        .clk_src = ADC_DIGI_CLK_SRC_DEFAULT,
    };
    esp_err_t ret = adc_oneshot_new_unit(&init_config, &adc_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_12,  // 11dB or 12dB for full range (up to ~3.3V)
    };
    ret = adc_oneshot_config_channel(adc_handle, VBAT_ADC_CHANNEL, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC channel config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

esp_err_t board_hal_prepare_for_sleep(void)
{
    ESP_LOGI(TAG, "Preparing EE02 for sleep");
    // Disable ADC to save power
    if (adc_handle) {
        adc_oneshot_del_unit(adc_handle);
        adc_handle = NULL;
    }
    return ESP_OK;
}

bool board_hal_is_battery_connected(void)
{
    return false;
}

int board_hal_get_battery_voltage(void)
{
    if (!adc_handle)
        return -1;

    int adc_raw;
    if (adc_oneshot_read(adc_handle, VBAT_ADC_CHANNEL, &adc_raw) == ESP_OK) {
        // Crude conversion without calibration curve for now.
        // ADC_ATTEN_DB_11 is roughly 3.3V full scale at 4096 (12bit).
        // Voltage = raw * (3300 / 4095) * divider
        float voltage_mv = (float) adc_raw * (3300.0f / 4095.0f) * VBAT_VOLTAGE_DIVIDER;
        return (int) voltage_mv;
    }
    return -1;
}

int board_hal_get_battery_percent(void)
{
    int voltage = board_hal_get_battery_voltage();
    if (voltage < 0)
        return -1;

    // Simple linear approximation for LiPo
    // 4.2V = 100%, 3.3V = 0%
    if (voltage >= 4200)
        return 100;
    if (voltage <= 3300)
        return 0;

    return (voltage - 3300) * 100 / (4200 - 3300);
}

bool board_hal_is_charging(void)
{
    // TODO: Need specific GPIO for CHG status if available.
    // Return false for now to be safe.
    return false;
}

bool board_hal_is_usb_connected(void)
{
    // TODO: Check VBUS or USB peripheral status.
    // For now returning true ensures logic doesn't aggressively sleep if debugging.
    return true;
}

void board_hal_shutdown(void)
{
    // BQ24070 doesn't have software shutdown command via I2C like AXP.
    // We can only enter deep sleep.
    ESP_LOGI(TAG, "Shutdown not supported on BQ24070, entering deep sleep instead");
    board_hal_prepare_for_sleep();
    esp_deep_sleep_start();
}

esp_err_t board_hal_rtc_init(void)
{
    // No external RTC on XIAO EE02, rely on internal
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t board_hal_rtc_get_time(time_t *t)
{
    // TODO: Could return internal time if desired, but main.c handles fallback.
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t board_hal_rtc_set_time(time_t t)
{
    return ESP_ERR_NOT_SUPPORTED;
}

bool board_hal_rtc_is_available(void)
{
    return false;
}

esp_err_t board_hal_get_temperature(float *t)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t board_hal_get_humidity(float *h)
{
    return ESP_ERR_NOT_SUPPORTED;
}

uint16_t board_hal_get_display_width(void)
{
    return epaper_get_width();
}

uint16_t board_hal_get_display_height(void)
{
    return epaper_get_height();
}

board_type_t board_hal_get_type(void)
{
    return BOARD_TYPE_SEEEDSTUDIO_XIAO_EE02;
}

const char *board_hal_get_name(void)
{
    return "seeedstudio_xiao_ee02";
}

uint16_t board_hal_get_display_rotation(void)
{
    return 0;
}
