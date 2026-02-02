#include <esp_timer.h>
#include <string.h>

#include "board_hal.h"
#include "driver/gpio.h"
#include "epaper_port.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/soc_caps.h"

static const char *TAG = "board_hal_reterminal_e1002";

// Battery measurement constants
#define VBAT_ADC_CHANNEL BOARD_HAL_BAT_ADC_PIN
// Divider ratio needs verify. Typical Seeed is 2.0 (100k/100k) or similar.
#define VBAT_VOLTAGE_DIVIDER 2.0f

static adc_oneshot_unit_handle_t adc_handle = NULL;

static void board_hal_battery_adc_init(void)
{
    if (adc_handle)
        return;

    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT_1,  // GPIOs 1-10 are typically ADC1 on S3, but check channel mapping
        .clk_src = ADC_DIGI_CLK_SRC_DEFAULT,
    };
    // GPIO 1 on S3 is ADC1 Channel 0.
    if (BOARD_HAL_BAT_ADC_PIN >= ADC_CHANNEL_0 && BOARD_HAL_BAT_ADC_PIN <= ADC_CHANNEL_9) {
        init_config.unit_id = ADC_UNIT_1;  // Simple assumption, verify if needed
    }

    esp_err_t ret = adc_oneshot_new_unit(&init_config, &adc_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC init failed: %s", esp_err_to_name(ret));
        return;
    }

    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_12,
    };
    ret = adc_oneshot_config_channel(adc_handle, VBAT_ADC_CHANNEL, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC channel config failed: %s", esp_err_to_name(ret));
    }
}

esp_err_t board_hal_init(void)
{
    ESP_LOGI(TAG, "Initializing reTerminal E1002 Power HAL");

    // Initialize SD Card Power (Turn ON)
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << BOARD_HAL_SD_PWR_PIN),
        .pull_down_en = 0,
        .pull_up_en = 0,
    };
    gpio_config(&io_conf);
    gpio_set_level(BOARD_HAL_SD_PWR_PIN, 1);
    ESP_LOGI(TAG, "SD Card Power ON");

    // Initialize E-Paper Display Port
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

    // Initialize Battery Enable Pin
    io_conf.pin_bit_mask = (1ULL << BOARD_HAL_BAT_EN_PIN);
    gpio_config(&io_conf);
    gpio_set_level(BOARD_HAL_BAT_EN_PIN, 0);  // Disable measurement by default

    // Initialize ADC handle
    board_hal_battery_adc_init();

    return ESP_OK;
}

esp_err_t board_hal_prepare_for_sleep(void)
{
    ESP_LOGI(TAG, "Preparing reTerminal E1002 for sleep");

    // Put display to deep sleep
    epaper_port_enter_deepsleep();

    // Turn off SD Power
    gpio_set_level(BOARD_HAL_SD_PWR_PIN, 0);

    // Disable Battery Measurement
    gpio_set_level(BOARD_HAL_BAT_EN_PIN, 0);

    // Disable ADC
    if (adc_handle) {
        adc_oneshot_del_unit(adc_handle);
        adc_handle = NULL;
    }

    return ESP_OK;
}

bool board_hal_is_battery_connected(void)
{
    // If voltage > 0, assumed connected
    return board_hal_get_battery_voltage() > 500;
}

int board_hal_get_battery_voltage(void)
{
    if (!adc_handle) {
        // Try to re-init if handle lost
        board_hal_battery_adc_init();
        if (!adc_handle)
            return -1;
    }

    // Enable measurement
    gpio_set_level(BOARD_HAL_BAT_EN_PIN, 1);
    // Wait for stabilization
    vTaskDelay(pdMS_TO_TICKS(10));

    int adc_raw;
    esp_err_t ret = adc_oneshot_read(adc_handle, VBAT_ADC_CHANNEL, &adc_raw);

    // Disable measurement
    gpio_set_level(BOARD_HAL_BAT_EN_PIN, 0);

    if (ret == ESP_OK) {
        // Voltage = raw * (3300 / 4095) * divider
        // Note: ADC_ATTEN_DB_12 covers up to ~3.1-3.3V depending on cal.
        // Approx: 0.8 mV per LSB * divider.
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

    // Simple Linear: 3.3V (0%) to 4.2V (100%)
    if (voltage >= 4200)
        return 100;
    if (voltage <= 3300)
        return 0;
    return (voltage - 3300) * 100 / (4200 - 3300);
}

bool board_hal_is_charging(void)
{
    return false;
}

bool board_hal_is_usb_connected(void)
{
    return true;
}

void board_hal_shutdown(void)
{
    ESP_LOGI(TAG, "Shutdown requested, entering deep sleep");
    board_hal_prepare_for_sleep();
    esp_deep_sleep_start();
}

esp_err_t board_hal_rtc_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t board_hal_rtc_get_time(time_t *t)
{
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
