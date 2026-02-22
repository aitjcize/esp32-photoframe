#include <esp_timer.h>

#include "board_hal.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "epaper.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "soc/soc_caps.h"

static const char *TAG = "board_hal_ee04";

#define VBAT_ADC_CHANNEL ADC_CHANNEL_0
#define VBAT_VOLTAGE_DIVIDER 2.0f

static adc_oneshot_unit_handle_t adc_handle = NULL;

esp_err_t board_hal_init(void)
{
    ESP_LOGI(TAG, "Initializing XIAO EE04 Power HAL (BQ24070)");

    ESP_LOGI(TAG, "Initializing SPI bus for 7.3 Panel...");
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = BOARD_HAL_SPI_MOSI_PIN,
        .miso_io_num = -1,
        .sclk_io_num = BOARD_HAL_SPI_SCLK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 800 * 480 / 2 + 100,  // Optimized for 7.3" EPD
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    epaper_config_t ep_cfg = {
        .spi_host = SPI2_HOST,
        .pin_cs = BOARD_HAL_EPD_CS_PIN,
        .pin_dc = BOARD_HAL_EPD_DC_PIN,
        .pin_rst = BOARD_HAL_EPD_RST_PIN,
        .pin_busy = BOARD_HAL_EPD_BUSY_PIN,
        .pin_cs1 = -1, // CS1 disabled for Standard-SPI
        .pin_enable = BOARD_HAL_EPD_ENABLE_PIN,
    };
    epaper_init(&ep_cfg);

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
        .atten = ADC_ATTEN_DB_12,
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
    ESP_LOGI(TAG, "Preparing EE04 for sleep");
    epaper_enter_deepsleep();
    if (adc_handle) {
        adc_oneshot_del_unit(adc_handle);
        adc_handle = NULL;
    }
    return ESP_OK;
}

bool board_hal_is_battery_connected(void) { return false; }

int board_hal_get_battery_voltage(void)
{
    if (!adc_handle) return -1;
    int adc_raw;
    if (adc_oneshot_read(adc_handle, VBAT_ADC_CHANNEL, &adc_raw) == ESP_OK) {
        float voltage_mv = (float) adc_raw * (3300.0f / 4095.0f) * VBAT_VOLTAGE_DIVIDER;
        return (int) voltage_mv;
    }
    return -1;
}

int board_hal_get_battery_percent(void)
{
    int voltage = board_hal_get_battery_voltage();
    if (voltage < 0) return -1;
    if (voltage >= 4200) return 100;
    if (voltage <= 3300) return 0;
    return (voltage - 3300) * 100 / (4200 - 3300);
}

bool board_hal_is_charging(void) { return false; }
bool board_hal_is_usb_connected(void) { return true; }

void board_hal_shutdown(void)
{
    ESP_LOGI(TAG, "Shutdown not supported on BQ24070, entering deep sleep instead");
    board_hal_prepare_for_sleep();
    esp_deep_sleep_start();
}

esp_err_t board_hal_rtc_init(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t board_hal_rtc_get_time(time_t *t) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t board_hal_rtc_set_time(time_t t) { return ESP_ERR_NOT_SUPPORTED; }
bool board_hal_rtc_is_available(void) { return false; }
esp_err_t board_hal_get_temperature(float *t) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t board_hal_get_humidity(float *h) { return ESP_ERR_NOT_SUPPORTED; }