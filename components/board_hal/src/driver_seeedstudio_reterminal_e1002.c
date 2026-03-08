#include "board_hal.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "driver/usb_serial_jtag.h"
#include "epaper.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pcf8563.h"
#include "sensor.h"

#ifdef CONFIG_HAS_SDCARD
#include "sdcard.h"
#endif

static const char *TAG = "board_hal_reterminal_e1002";

static i2c_master_bus_handle_t i2c_bus = NULL;

// Battery measurement constants
#define VBAT_ADC_CHANNEL BOARD_HAL_BAT_ADC_PIN
// Voltage divider ratio: 2.0 (100k/100k resistor divider)
#define VBAT_VOLTAGE_DIVIDER 2.0f

static adc_oneshot_unit_handle_t adc_handle = NULL;

static void board_hal_battery_adc_init(void)
{
    if (adc_handle)
        return;

    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT_1,  // GPIO 1 = ADC1 Channel 0
        .clk_src = ADC_DIGI_CLK_SRC_DEFAULT,
    };

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
        adc_oneshot_del_unit(adc_handle);
        adc_handle = NULL;
        return;
    }
}

esp_err_t board_hal_init(void)
{
    ESP_LOGI(TAG, "Initializing reTerminal E1002 Board HAL");

    // --- SPI bus ---
    ESP_LOGI(TAG, "Initializing SPI bus...");

    // Pull CS pins HIGH early to prevent interference on the shared bus
    gpio_config_t cs_cfg = {
        .pin_bit_mask = (1ULL << BOARD_HAL_EPD_CS_PIN) | (1ULL << BOARD_HAL_SD_CS_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&cs_cfg);
    gpio_set_level(BOARD_HAL_EPD_CS_PIN, 1);
    gpio_set_level(BOARD_HAL_SD_CS_PIN, 1);

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = BOARD_HAL_SPI_MOSI_PIN,
        .miso_io_num = BOARD_HAL_SPI_MISO_PIN,
        .sclk_io_num = BOARD_HAL_SPI_SCLK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4092,
        .flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_SCLK,
    };

    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    // --- E-Paper Display ---
    epaper_config_t ep_cfg = {
        .spi_host = SPI2_HOST,
        .pin_cs = BOARD_HAL_EPD_CS_PIN,
        .pin_dc = BOARD_HAL_EPD_DC_PIN,
        .pin_rst = BOARD_HAL_EPD_RST_PIN,
        .pin_busy = BOARD_HAL_EPD_BUSY_PIN,
        .pin_cs1 = -1,
        .pin_enable = -1,
    };
    epaper_init(&ep_cfg);

    // --- SD Card ---
#ifdef CONFIG_HAS_SDCARD
    gpio_config_t sd_pwr_cfg = {
        .pin_bit_mask = (1ULL << BOARD_HAL_SD_PWR_PIN),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&sd_pwr_cfg);
    gpio_set_level(BOARD_HAL_SD_PWR_PIN, 1);
    ESP_LOGI(TAG, "SD Card Power ON");

    // Give SD card time to power up and stabilize
    vTaskDelay(pdMS_TO_TICKS(500));

    ESP_LOGI(TAG, "Initializing SD card (SPI)...");
    sdcard_config_t sd_cfg = {
        .mount_point = "/storage",
        .host_id = SPI2_HOST,
        .cs_pin = BOARD_HAL_SD_CS_PIN,
    };

    esp_err_t sd_ret = sdcard_init(&sd_cfg);
    if (sd_ret == ESP_OK) {
        ESP_LOGI(TAG, "SD card initialized successfully");
    } else {
        ESP_LOGW(TAG, "SD card initialization failed: %s", esp_err_to_name(sd_ret));
    }
#endif

    // --- Battery ADC ---
    gpio_config_t bat_en_cfg = {
        .pin_bit_mask = (1ULL << BOARD_HAL_BAT_EN_PIN),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&bat_en_cfg);
    gpio_set_level(BOARD_HAL_BAT_EN_PIN, 0);  // Disable measurement by default

    board_hal_battery_adc_init();

    // --- Onboard LED (GPIO6, active-low) ---
    gpio_config_t led_cfg = {
        .pin_bit_mask = (1ULL << BOARD_HAL_LED_PIN),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&led_cfg);
    board_hal_led_set(BOARD_HAL_LED_ACTIVITY, false);

    // --- I2C bus ---
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
        // Initialize SHT40 temperature/humidity sensor
        if (sensor_init(i2c_bus) == ESP_OK) {
            ESP_LOGI(TAG, "SHT40 sensor initialized");
        }

        // Initialize PCF8563T RTC
        if (pcf8563_init(i2c_bus) == ESP_OK) {
            ESP_LOGI(TAG, "PCF8563T RTC initialized");
        }
    } else {
        ESP_LOGE(TAG, "Failed to initialize I2C bus: %s", esp_err_to_name(i2c_ret));
    }

    return ESP_OK;
}

esp_err_t board_hal_prepare_for_sleep(void)
{
    ESP_LOGI(TAG, "Preparing reTerminal E1002 for sleep");

    // Turn off LED
    board_hal_led_set(BOARD_HAL_LED_ACTIVITY, false);

    // Put display to deep sleep
    epaper_enter_deepsleep();

    // Turn off SD Power
#ifdef CONFIG_HAS_SDCARD
    gpio_set_level(BOARD_HAL_SD_PWR_PIN, 0);
#endif

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

    // Calibrated voltage-to-percent mapping (LiPo discharge curve)
    static const struct {
        int mv;
        int pct;
    } cal[] = {
        {4150, 100}, {3960, 90}, {3910, 80}, {3850, 70}, {3800, 60}, {3750, 50},
        {3680, 40},  {3580, 30}, {3490, 20}, {3410, 10}, {3300, 5},  {3270, 0},
    };

    if (voltage >= cal[0].mv)
        return 100;
    if (voltage <= cal[sizeof(cal) / sizeof(cal[0]) - 1].mv)
        return 0;

    // Linear interpolation between calibration points
    for (int i = 0; i < (int) (sizeof(cal) / sizeof(cal[0])) - 1; i++) {
        if (voltage >= cal[i + 1].mv) {
            int dv = cal[i].mv - cal[i + 1].mv;
            int dp = cal[i].pct - cal[i + 1].pct;
            return cal[i + 1].pct + (voltage - cal[i + 1].mv) * dp / dv;
        }
    }
    return 0;
}

bool board_hal_is_charging(void)
{
    return false;
}

bool board_hal_is_usb_connected(void)
{
    return usb_serial_jtag_is_connected();
}

void board_hal_shutdown(void)
{
    ESP_LOGI(TAG, "Shutdown requested, entering deep sleep");
    board_hal_prepare_for_sleep();
    esp_deep_sleep_start();
}

esp_err_t board_hal_rtc_init(void)
{
    return pcf8563_is_available() ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t board_hal_rtc_get_time(time_t *t)
{
    return pcf8563_read_time(t);
}

esp_err_t board_hal_rtc_set_time(time_t t)
{
    return pcf8563_write_time(t);
}

bool board_hal_rtc_is_available(void)
{
    return pcf8563_is_available();
}

void board_hal_led_set(board_hal_led_t led, bool on)
{
    // reTerminal E1002 has a single LED — use it for both status and activity
    (void) led;
    gpio_set_level(BOARD_HAL_LED_PIN, BOARD_HAL_LED_INVERTED ? !on : on);
}

esp_err_t board_hal_get_temperature(float *t)
{
    if (!t)
        return ESP_ERR_INVALID_ARG;
    if (!sensor_is_available())
        return ESP_ERR_INVALID_STATE;

    float h_dummy;
    return sensor_read(t, &h_dummy);
}

esp_err_t board_hal_get_humidity(float *h)
{
    if (!h)
        return ESP_ERR_INVALID_ARG;
    if (!sensor_is_available())
        return ESP_ERR_INVALID_STATE;

    float t_dummy;
    return sensor_read(&t_dummy, h);
}
