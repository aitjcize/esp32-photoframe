#include <esp_timer.h>
#include <string.h>

#include "board_hal.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "epaper_port.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sht40_sensor.h"
#include "soc/soc_caps.h"

#ifdef CONFIG_HAS_SDCARD
#include "sdcard.h"
#endif

static const char *TAG = "board_hal_reterminal_e1002";

static i2c_master_bus_handle_t i2c_bus = NULL;

// I2C Pins for reTerminal E1002 (TP_INT=3, TP_RST=4, SDA=5, SCL=6)
#define BOARD_HAL_I2C_SDA_PIN 5
#define BOARD_HAL_I2C_SCL_PIN 6

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

#ifdef CONFIG_HAS_SDCARD
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

    // Initialize SD card (SPI interface for reTerminal E1002)
    ESP_LOGI(TAG, "Initializing SD card (SPI)...");
    sdcard_spi_config_t sd_config = {
        .cs_pin = GPIO_NUM_14,
        .mosi_pin = GPIO_NUM_9,
        .miso_pin = GPIO_NUM_8,
        .sclk_pin = GPIO_NUM_7,
    };

    esp_err_t sd_ret = sdcard_init_spi(&sd_config);
    if (sd_ret == ESP_OK) {
        ESP_LOGI(TAG, "SD card initialized successfully");
    } else {
        ESP_LOGW(TAG, "SD card initialization failed: %s", esp_err_to_name(sd_ret));
    }
#endif

    // Initialize Battery Enable Pin
    io_conf.pin_bit_mask = (1ULL << BOARD_HAL_BAT_EN_PIN);
    gpio_config(&io_conf);
    gpio_set_level(BOARD_HAL_BAT_EN_PIN, 0);  // Disable measurement by default

    // Initialize ADC handle
    board_hal_battery_adc_init();

    // Initialize I2C Bus
    i2c_master_bus_config_t i2c_bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = 0,
        .scl_io_num = BOARD_HAL_I2C_SCL_PIN,
        .sda_io_num = BOARD_HAL_I2C_SDA_PIN,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t i2c_ret = i2c_new_master_bus(&i2c_bus_config, &i2c_bus);
    if (i2c_ret == ESP_OK) {
        ESP_LOGI(TAG, "I2C bus initialized");

        // Initialize SHT40 Sensor
        if (sht40_init(i2c_bus) == ESP_OK) {
            ESP_LOGI(TAG, "SHT40 sensor initialized");
        }
    } else {
        ESP_LOGE(TAG, "Failed to initialize I2C bus: %s", esp_err_to_name(i2c_ret));
    }

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
    if (!t)
        return ESP_ERR_INVALID_ARG;
    if (!sht40_is_available())
        return ESP_ERR_INVALID_STATE;

    float h_dummy;
    return sht40_read(t, &h_dummy);
}
esp_err_t board_hal_get_humidity(float *h)
{
    if (!h)
        return ESP_ERR_INVALID_ARG;
    if (!sht40_is_available())
        return ESP_ERR_INVALID_STATE;

    float t_dummy;
    return sht40_read(&t_dummy, h);
}
