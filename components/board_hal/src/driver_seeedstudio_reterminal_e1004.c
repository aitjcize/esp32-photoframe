#include "board_hal.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "driver/usb_serial_jtag.h"
#include "epaper.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
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

static const char *TAG = "board_hal_reterminal_e1004";

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

// ADC samples averaged per reading to reduce noise.
#define VBAT_ADC_SAMPLES 8

static adc_oneshot_unit_handle_t adc_handle = NULL;
static adc_cali_handle_t adc_cali_handle = NULL;

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

    // eFuse-based calibration converts raw counts to accurate millivolts; the
    // ESP32-S3 ADC is nonlinear, so the raw*3300/4095 estimate drifts.
    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,
        .chan = VBAT_ADC_CHANNEL,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_config, &adc_cali_handle) != ESP_OK) {
        ESP_LOGW(TAG, "ADC calibration unavailable; using linear estimate");
        adc_cali_handle = NULL;
    }
}

esp_err_t board_hal_init(void)
{
    ESP_LOGI(TAG, "Initializing reTerminal E1004 Board HAL");

    // Release any pad holds latched by the previous deep-sleep cycle so we
    // can reconfigure these outputs during init. Matching `gpio_hold_en()`
    // calls in board_hal_prepare_for_sleep() re-apply the hold before sleep.
    gpio_hold_dis(BOARD_HAL_LED_PIN);
    gpio_hold_dis(BOARD_HAL_BAT_EN_PIN);
#ifdef CONFIG_HAS_SDCARD
    gpio_hold_dis(BOARD_HAL_SD_PWR_PIN);
#endif

    // --- SPI bus ---
    ESP_LOGI(TAG, "Initializing SPI bus...");

    // Pull CS pins HIGH early to prevent interference on the shared bus.
    // The 13.3" panel is split into two halves, each with its own chip
    // select (CS = master half, CS1 = slave half).
    gpio_config_t cs_cfg = {
        .pin_bit_mask = (1ULL << BOARD_HAL_EPD_CS_PIN) | (1ULL << BOARD_HAL_EPD_CS1_PIN) |
                        (1ULL << BOARD_HAL_SD_CS_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&cs_cfg);
    gpio_set_level(BOARD_HAL_EPD_CS_PIN, 1);
    gpio_set_level(BOARD_HAL_EPD_CS1_PIN, 1);
    gpio_set_level(BOARD_HAL_SD_CS_PIN, 1);

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = BOARD_HAL_SPI_MOSI_PIN,
        .miso_io_num = BOARD_HAL_SPI_MISO_PIN,
        .sclk_io_num = BOARD_HAL_SPI_SCLK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        // The NCA panel driver streams the framebuffer line-by-line (<=300
        // bytes per SPI transaction), so a small DMA buffer is plenty. A huge
        // max_transfer_sz on this SD-shared bus breaks SD sector-read DMA
        // (ESP_ERR_TIMEOUT / 0x107); match the E1002, which shares SD and the
        // panel on the same bus and works at the default SD clock.
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
        .pin_cs1 = BOARD_HAL_EPD_CS1_PIN,
        // SCREEN_EN# drives an active-high load switch for the panel's 3V3
        // rail. The ED2208 NCA driver enables it on init and cuts it (with a
        // deep-sleep pad hold) in epaper_enter_deepsleep().
        .pin_enable = BOARD_HAL_EPD_ENABLE_PIN,
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

        // Initialize SY6974B charger (shares I2C0 with the RTC/sensor).
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
    ESP_LOGI(TAG, "Preparing reTerminal E1004 for sleep");

    // Put SHT40 sensor to sleep before we tear down the I2C bus
    if (sensor_is_available()) {
        sensor_sleep();
        ESP_LOGI(TAG, "SHT40 sensor put to sleep");
    }

    // Turn off LED
    board_hal_led_set(BOARD_HAL_LED_ACTIVITY, false);

    // Put display to deep sleep
    epaper_enter_deepsleep();

    // Unmount SD card and release the SPI bus before cutting power so the
    // card sees a clean shutdown instead of a VCC yank.
#ifdef CONFIG_HAS_SDCARD
    sdcard_deinit();
    gpio_set_level(BOARD_HAL_SD_PWR_PIN, 0);
#endif

    // Disable Battery Measurement
    gpio_set_level(BOARD_HAL_BAT_EN_PIN, 0);

    // Disable ADC
    if (adc_handle) {
        adc_oneshot_del_unit(adc_handle);
        adc_handle = NULL;
    }

    // Latch the output pins so their levels survive into deep sleep instead
    // of floating once the digital IO domain powers down.
    gpio_hold_en(BOARD_HAL_LED_PIN);
    gpio_hold_en(BOARD_HAL_BAT_EN_PIN);
#ifdef CONFIG_HAS_SDCARD
    gpio_hold_en(BOARD_HAL_SD_PWR_PIN);
#endif
    gpio_deep_sleep_hold_en();

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

    // Average several samples to reduce noise.
    int64_t raw_sum = 0;
    int samples = 0;
    for (int i = 0; i < VBAT_ADC_SAMPLES; i++) {
        int adc_raw;
        if (adc_oneshot_read(adc_handle, VBAT_ADC_CHANNEL, &adc_raw) == ESP_OK) {
            raw_sum += adc_raw;
            samples++;
        }
    }

    // Disable measurement
    gpio_set_level(BOARD_HAL_BAT_EN_PIN, 0);

    if (samples == 0)
        return -1;
    int raw_avg = (int) (raw_sum / samples);

    int pin_mv;
    if (adc_cali_handle && adc_cali_raw_to_voltage(adc_cali_handle, raw_avg, &pin_mv) == ESP_OK) {
        // Calibrated mV at the ADC pin; undo the divider + apply the optional
        // multimeter correction.
        return (int) ((float) pin_mv * VBAT_VOLTAGE_DIVIDER * VBAT_CAL_SCALE);
    }
    // Fallback: uncalibrated linear estimate.
    return (int) ((float) raw_avg * (3300.0f / 4095.0f) * VBAT_VOLTAGE_DIVIDER * VBAT_CAL_SCALE);
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
    // Real charge state from the SY6974B; false if the charger didn't probe.
    return sy6974b_is_charging();
}

bool board_hal_is_usb_connected(void)
{
    // Prefer the charger's power-good (detects any USB/adapter input, even a
    // data-less charger); fall back to USB-serial-JTAG if the charger is absent.
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
    // reTerminal E1004 has a single LED — use it for both status and activity
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
