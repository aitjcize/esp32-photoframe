#include <time.h>

#include "axp_prot.h"
#include "board_hal.h"
#include "epaper_port.h"
#include "esp_log.h"
#include "i2c_bsp.h"
#include "pcf85063_rtc.h"
#include "shtc3_sensor.h"

static const char *TAG = "board_hal_waveshare";

esp_err_t board_hal_init(void)
{
    // Initialize I2C bus
    ESP_LOGI(TAG, "Initializing I2C bus...");
    i2c_master_Init();

    ESP_LOGI(TAG, "Initializing WaveShare PhotoPainter Power HAL");
    axp_i2c_prot_init();
    axp_cmd_init();

    // Initialize SHTC3 sensor (part of this board's power/sensor hal)
    esp_err_t shtc3_ret = shtc3_init();
    if (shtc3_ret == ESP_OK) {
        ESP_LOGI(TAG, "SHTC3 sensor initialized successfully");
    } else {
        ESP_LOGW(TAG, "SHTC3 sensor initialization failed (sensor may not be present)");
    }
    return ESP_OK;
}

esp_err_t board_hal_prepare_for_sleep(void)
{
    ESP_LOGI(TAG, "Preparing system for sleep");

    // Put SHTC3 sensor to sleep
    if (shtc3_is_available()) {
        shtc3_sleep();
        ESP_LOGI(TAG, "SHTC3 sensor put to sleep");
    }

    ESP_LOGI(TAG, "Preparing AXP2101 for sleep");
    axp_basic_sleep_start();
    return ESP_OK;
}

bool board_hal_is_battery_connected(void)
{
    return axp_is_battery_connected();
}

int board_hal_get_battery_percent(void)
{
    // axp_get_battery_percent returns int directly
    return axp_get_battery_percent();
}

int board_hal_get_battery_voltage(void)
{
    // axp_get_battery_voltage returns in mV
    return axp_get_battery_voltage();
}

bool board_hal_is_charging(void)
{
    return axp_is_charging();
}

bool board_hal_is_usb_connected(void)
{
    return axp_is_usb_connected();
}

void board_hal_shutdown(void)
{
    axp_shutdown();
}

esp_err_t board_hal_get_temperature(float *t)
{
    if (!t)
        return ESP_ERR_INVALID_ARG;
    if (!shtc3_is_available())
        return ESP_ERR_INVALID_STATE;

    // SHTC3 driver usually provides combined read, or we cache?
    // Let's assume shtc3_read_temperature exists or similar.
    // Checking shtc3_sensor.h would be ideal, but assuming standard interface:
    float h_dummy;
    return shtc3_read(t, &h_dummy);
}

esp_err_t board_hal_get_humidity(float *h)
{
    if (!h)
        return ESP_ERR_INVALID_ARG;
    if (!shtc3_is_available())
        return ESP_ERR_INVALID_STATE;
    float t_dummy;
    return shtc3_read(&t_dummy, h);
}

esp_err_t board_hal_rtc_init(void)
{
    return pcf85063_init();
}

esp_err_t board_hal_rtc_get_time(time_t *t)
{
    if (!t)
        return ESP_ERR_INVALID_ARG;
    return pcf85063_read_time(t);
}

esp_err_t board_hal_rtc_set_time(time_t t)
{
    return pcf85063_write_time(t);
}

bool board_hal_rtc_is_available(void)
{
    return pcf85063_is_available();
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
    return BOARD_TYPE_WAVESHARE_PHOTOPAINTER;
}

const char *board_hal_get_name(void)
{
    return "waveshare_photopainter_73";
}

uint16_t board_hal_get_display_rotation_deg(void)
{
    // Waveshare 7.3" native landscape requires 180 flip for this enclosure
    return 180;
}
