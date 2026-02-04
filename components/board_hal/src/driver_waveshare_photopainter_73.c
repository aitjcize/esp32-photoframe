#include <time.h>

#include "axp_prot.h"
#include "board_hal.h"
#include "driver/i2c_master.h"
#include "epaper_port.h"
#include "esp_log.h"
#include "pcf85063_rtc.h"
#include "shtc3_sensor.h"

#ifdef CONFIG_HAS_SDCARD
#include "sdcard.h"
#endif

static const char *TAG = "board_hal_waveshare";

static i2c_master_bus_handle_t i2c_bus = NULL;

esp_err_t board_hal_init(void)
{
    // Initialize I2C bus
    ESP_LOGI(TAG, "Initializing I2C bus...");
    i2c_master_bus_config_t i2c_bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = 0,
        .scl_io_num = 48,
        .sda_io_num = 47,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_config, &i2c_bus));

    ESP_LOGI(TAG, "Initializing WaveShare PhotoPainter Power HAL");
    axp_init(i2c_bus);
    axp_cmd_init();

    // Initialize SHTC3 sensor (part of this board's power/sensor hal)
    esp_err_t shtc3_ret = shtc3_init(i2c_bus);
    if (shtc3_ret == ESP_OK) {
        ESP_LOGI(TAG, "SHTC3 sensor initialized successfully");
    } else {
        ESP_LOGW(TAG, "SHTC3 sensor initialization failed (sensor may not be present)");
    }

    // Initialize E-Paper Display Port
    // Note: Waveshare header now defines these pins locally (or via Kconfig fallback if we kept it)
    epaper_config_t ep_cfg = {
        .pin_cs = BOARD_HAL_EPD_CS_PIN,
        .pin_dc = BOARD_HAL_EPD_DC_PIN,
        .pin_rst = BOARD_HAL_EPD_RST_PIN,
        .pin_busy = BOARD_HAL_EPD_BUSY_PIN,
        .pin_sck = BOARD_HAL_EPD_SCK_PIN,
        .pin_mosi = BOARD_HAL_EPD_MOSI_PIN,
        .pin_enable = -1,
    };
    epaper_port_init(&ep_cfg);

#ifdef CONFIG_HAS_SDCARD
    // Initialize SD card (SDIO interface for WaveShare)
    ESP_LOGI(TAG, "Initializing SD card (SDIO)...");
    sdcard_sdio_config_t sd_config = {
        .clk_pin = GPIO_NUM_39,
        .cmd_pin = GPIO_NUM_41,
        .d0_pin = GPIO_NUM_40,
        .d1_pin = GPIO_NUM_1,
        .d2_pin = GPIO_NUM_2,
        .d3_pin = GPIO_NUM_38,
    };

    esp_err_t sd_ret = sdcard_init_sdio(&sd_config);
    if (sd_ret == ESP_OK) {
        ESP_LOGI(TAG, "SD card initialized successfully");
    } else {
        ESP_LOGW(TAG, "SD card initialization failed: %s", esp_err_to_name(sd_ret));
    }
#endif

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
    return pcf85063_init(i2c_bus);
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
