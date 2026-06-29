#include <math.h>

#include "battery_adc.h"
#include "board_hal.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "driver/usb_serial_jtag.h"
#include "epaper.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pcf8563.h"
#include "sensor.h"
#include "sy6974b.h"

#ifdef CONFIG_HAS_SDCARD
#include "sdcard.h"
#endif

static const char *TAG = "board_hal_reterminal_e1003";

static i2c_master_bus_handle_t i2c_bus = NULL;

// Battery measurement constants
#define VBAT_ADC_CHANNEL BOARD_HAL_BAT_ADC_PIN
// Voltage divider ratio: 2.0 (100k/100k resistor divider)
#define VBAT_VOLTAGE_DIVIDER 2.0f

// Optional per-unit correction for resistor-divider tolerance, measured with a
// multimeter: set to (multimeter_mV / firmware_reported_mV). 1.0 = none.
#ifndef VBAT_CAL_SCALE
#define VBAT_CAL_SCALE 1.0f
#endif

static battery_adc_t *vbat_adc = NULL;

static void board_hal_battery_adc_init(void)
{
    if (vbat_adc)
        return;

    // GPIO1 (ADC1_CH0) behind a 2:1 divider gated by BAT_EN; the shared helper
    // handles eFuse calibration + averaging.
    battery_adc_config_t cfg = {
        .unit = ADC_UNIT_1,
        .channel = VBAT_ADC_CHANNEL,
        .atten = ADC_ATTEN_DB_12,
        .enable_pin = BOARD_HAL_BAT_EN_PIN,
        .settle_ms = 10,
        .samples = 8,
        .divider = VBAT_VOLTAGE_DIVIDER,
        .cal_scale = VBAT_CAL_SCALE,
    };
    battery_adc_create(&cfg, &vbat_adc);
}

esp_err_t board_hal_init(void)
{
    ESP_LOGI(TAG, "Initializing reTerminal E1003 Board HAL");

    // Release any pad holds latched by the previous deep-sleep cycle.
    gpio_hold_dis(BOARD_HAL_LED_PIN);
    gpio_hold_dis(BOARD_HAL_BAT_EN_PIN);
    gpio_hold_dis(BOARD_HAL_EPD_VCC_EN_PIN);
#ifdef CONFIG_HAS_SDCARD
    gpio_hold_dis(BOARD_HAL_SD_PWR_PIN);
#endif

    // --- Power the IT8951 controller (ITE_VCC_EN) before any SPI access ---
    gpio_config_t vcc_cfg = {
        .pin_bit_mask = (1ULL << BOARD_HAL_EPD_VCC_EN_PIN),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&vcc_cfg);
    gpio_set_level(BOARD_HAL_EPD_VCC_EN_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(50));  // let the IT8951 3V3/1V8 rails settle

    // --- SPI bus (shared by the IT8951 and the microSD card) ---
    ESP_LOGI(TAG, "Initializing SPI bus...");

    // Pull CS pins HIGH early to avoid contention on the shared bus.
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
        // The IT8951 driver streams image data in <=4000-byte bursts; keep the
        // DMA buffer small so SD sector-read DMA on the shared bus is unaffected
        // (matches the E1002/E1004 SD-shared-bus configuration).
        .max_transfer_sz = 4092,
        .flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_SCLK,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    // --- E-Paper Display (IT8951) ---
    epaper_config_t ep_cfg = {
        .spi_host = SPI2_HOST,
        .pin_cs = BOARD_HAL_EPD_CS_PIN,
        .pin_dc = BOARD_HAL_EPD_DC_PIN,  // unused on IT8951
        .pin_rst = BOARD_HAL_EPD_RST_PIN,
        .pin_busy = BOARD_HAL_EPD_BUSY_PIN,      // HRDY
        .pin_cs1 = BOARD_HAL_EPD_CS1_PIN,        // unused
        .pin_enable = BOARD_HAL_EPD_ENABLE_PIN,  // EPD_Drive_EN (TPS65185 bias)
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

    // --- Onboard LED ---
    gpio_config_t led_cfg = {
        .pin_bit_mask = (1ULL << BOARD_HAL_LED_PIN),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&led_cfg);
    board_hal_led_set(BOARD_HAL_LED_ACTIVITY, false);

    // --- I2C bus (RTC + sensor + charger on I2C0) ---
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
        if (sensor_init(i2c_bus) == ESP_OK) {
            ESP_LOGI(TAG, "SHT40 sensor initialized");
            // Feed the live panel temperature to the IT8951 so it selects the
            // right grayscale waveform (better tone, less ghosting than a fixed
            // default). Re-read each wake, so deep-sleep cycles stay accurate.
            float temp_c = 0.0f, humidity = 0.0f;
            if (sensor_read(&temp_c, &humidity) == ESP_OK) {
                epaper_set_temperature((int8_t) lroundf(temp_c));
                ESP_LOGI(TAG, "Panel temperature set to %.1f C", temp_c);
            }
        }
        if (pcf8563_init(i2c_bus) == ESP_OK) {
            ESP_LOGI(TAG, "PCF8563T RTC initialized");
        }
        // SY6974B charger shares I2C0 with the RTC/sensor.
        if (sy6974b_init(i2c_bus) == ESP_OK) {
            ESP_LOGI(TAG, "SY6974B charger initialized");
        }
    } else {
        ESP_LOGE(TAG, "Failed to initialize I2C bus: %s", esp_err_to_name(i2c_ret));
    }

    return ESP_OK;
}

esp_err_t board_hal_prepare_for_sleep(void)
{
    ESP_LOGI(TAG, "Preparing reTerminal E1003 for sleep");

    if (sensor_is_available()) {
        sensor_sleep();
        ESP_LOGI(TAG, "SHT40 sensor put to sleep");
    }

    board_hal_led_set(BOARD_HAL_LED_ACTIVITY, false);

    // Put the IT8951 to sleep + drop the EPD bias rail (epaper_enter_deepsleep
    // cuts EPD_Drive_EN), then cut the IT8951 core power.
    epaper_enter_deepsleep();
    gpio_set_level(BOARD_HAL_EPD_VCC_EN_PIN, 0);

#ifdef CONFIG_HAS_SDCARD
    sdcard_deinit();
    gpio_set_level(BOARD_HAL_SD_PWR_PIN, 0);
#endif

    gpio_set_level(BOARD_HAL_BAT_EN_PIN, 0);
    battery_adc_destroy(vbat_adc);
    vbat_adc = NULL;

    // Latch output levels through deep sleep.
    gpio_hold_en(BOARD_HAL_LED_PIN);
    gpio_hold_en(BOARD_HAL_BAT_EN_PIN);
    gpio_hold_en(BOARD_HAL_EPD_VCC_EN_PIN);
#ifdef CONFIG_HAS_SDCARD
    gpio_hold_en(BOARD_HAL_SD_PWR_PIN);
#endif
    gpio_deep_sleep_hold_en();

    return ESP_OK;
}

bool board_hal_is_battery_connected(void)
{
    return board_hal_get_battery_voltage() > 500;
}

int board_hal_get_battery_voltage(void)
{
    if (!vbat_adc) {
        board_hal_battery_adc_init();
        if (!vbat_adc)
            return -1;
    }
    return battery_adc_read_mv(vbat_adc);
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
    // Real charge state from the SY6974B; false if the charger didn't probe.
    return sy6974b_is_charging();
}

bool board_hal_is_usb_connected(void)
{
    // Prefer the charger's power-good (detects any USB/adapter input); fall back
    // to USB-serial-JTAG if the charger is absent.
    if (sy6974b_is_available())
        return sy6974b_is_power_good();
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
