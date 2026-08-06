// Board HAL for the Seeed Studio XIAO ePaper Display Board EE03: a XIAO
// ESP32-S3 Plus driving a 10.3" monochrome IT8951 panel (1872x1404, 16-level
// grayscale / GC16). Same panel/controller as the reTerminal E1003, but on the
// XIAO ePaper driver-board wiring (like the EE02/EE04 boards).

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
#include "sensor.h"

static const char *TAG = "board_hal_ee03";

// Battery sense: GPIO1 (ADC1_CH0) behind a 2:1 divider gated by a TPS22916 load
// switch on GPIO6 (same circuit as the XIAO EE02/EE04 boards).
#define VBAT_ADC_CHANNEL ADC_CHANNEL_0
#define VBAT_ADC_ENABLE_PIN GPIO_NUM_6
#define VBAT_VOLTAGE_DIVIDER 2.0f

// Optional per-unit correction for resistor-divider tolerance, measured with a
// multimeter: set to (multimeter_mV / firmware_reported_mV). 1.0 = none.
#ifndef VBAT_CAL_SCALE
#define VBAT_CAL_SCALE 1.0f
#endif

static battery_adc_t *vbat_adc = NULL;
static i2c_master_bus_handle_t i2c_bus = NULL;

esp_err_t board_hal_init(void)
{
    ESP_LOGI(TAG, "Initializing XIAO EE03 (10.3\" IT8951) Board HAL");

    // Release any pad hold latched by the previous deep-sleep cycle so we can
    // reconfigure the battery load-switch, LED, PWR_EN, and IT8951 bus pins.
    gpio_hold_dis(VBAT_ADC_ENABLE_PIN);
    gpio_hold_dis(BOARD_HAL_LED_PIN);
    gpio_hold_dis(BOARD_HAL_PWR_EN_PIN);
    gpio_hold_dis(BOARD_HAL_EPD_CS_PIN);
    gpio_hold_dis(BOARD_HAL_SPI_SCLK_PIN);
    gpio_hold_dis(BOARD_HAL_SPI_MOSI_PIN);
    gpio_hold_dis(BOARD_HAL_EPD_RST_PIN);

    // --- SPI bus (IT8951 only; no microSD on this board) ---
    // MISO must be wired: the IT8951 is read back (GetSystemInfo / registers).
    ESP_LOGI(TAG, "Initializing SPI bus...");
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = BOARD_HAL_SPI_MOSI_PIN,
        .miso_io_num = BOARD_HAL_SPI_MISO_PIN,
        .sclk_io_num = BOARD_HAL_SPI_SCLK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        // The IT8951 driver streams image data in <=4000-byte bursts.
        .max_transfer_sz = 4092,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    // --- E-Paper Display (IT8951) ---
    // epaper_init() drives RST + PWR_EN high (powering the subsystem, including
    // VCC_3V3 for the SHT40 below), then resets and reads device info.
    epaper_config_t ep_cfg = {
        .spi_host = SPI2_HOST,
        .pin_cs = BOARD_HAL_EPD_CS_PIN,
        .pin_dc = BOARD_HAL_EPD_DC_PIN,          // unused on IT8951
        .pin_rst = BOARD_HAL_EPD_RST_PIN,
        .pin_busy = BOARD_HAL_EPD_BUSY_PIN,      // HRDY
        .pin_cs1 = BOARD_HAL_EPD_CS1_PIN,        // unused (single panel)
        .pin_enable = BOARD_HAL_PWR_EN_PIN,      // PWR_EN: master peripheral power
    };
    epaper_init(&ep_cfg);

    // --- Battery ADC (GPIO6 load switch, default disabled) ---
    gpio_config_t bat_en_cfg = {
        .pin_bit_mask = (1ULL << VBAT_ADC_ENABLE_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&bat_en_cfg));
    gpio_set_level(VBAT_ADC_ENABLE_PIN, 0);

    battery_adc_config_t vbat_cfg = {
        .unit = ADC_UNIT_1,
        .channel = VBAT_ADC_CHANNEL,
        .atten = ADC_ATTEN_DB_12,
        .enable_pin = VBAT_ADC_ENABLE_PIN,
        .settle_ms = 10,
        .samples = 8,
        .divider = VBAT_VOLTAGE_DIVIDER,
        .cal_scale = VBAT_CAL_SCALE,
    };
    battery_adc_create(&vbat_cfg, &vbat_adc);

    // --- Onboard LED (XIAO user LED, GPIO21, active-low) ---
    gpio_config_t led_cfg = {
        .pin_bit_mask = (1ULL << BOARD_HAL_LED_PIN),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&led_cfg);
    board_hal_led_set(BOARD_HAL_LED_ACTIVITY, false);

    // --- I2C bus (SHT40 temperature/humidity sensor at 0x44) ---
    // Feeds live panel temperature to the IT8951 for better waveform selection
    // (less ghosting than the fixed default).
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
            // Re-read each wake so deep-sleep cycles stay accurate.
            float temp_c = 0.0f, humidity = 0.0f;
            if (sensor_read(&temp_c, &humidity) == ESP_OK) {
                epaper_set_temperature((int8_t) lroundf(temp_c));
                ESP_LOGI(TAG, "Panel temperature set to %.1f C", temp_c);
            }
        } else {
            ESP_LOGW(TAG, "SHT40 sensor not found; using fixed waveform temperature");
        }
    } else {
        ESP_LOGE(TAG, "Failed to initialize I2C bus: %s", esp_err_to_name(i2c_ret));
    }

    return ESP_OK;
}

esp_err_t board_hal_prepare_for_sleep(void)
{
    ESP_LOGI(TAG, "Preparing EE03 for sleep");

    if (sensor_is_available()) {
        sensor_sleep();
        ESP_LOGI(TAG, "SHT40 sensor put to sleep");
    }

    board_hal_led_set(BOARD_HAL_LED_ACTIVITY, false);

    // Sleep the IT8951, then cut its power domain. epaper_enter_deepsleep drives
    // PWR_EN low, powering down the whole subsystem (else it draws several mA).
    epaper_enter_deepsleep();

    // With the IT8951 rail now cut, any of its bus pins left HIGH back-feeds the
    // dead chip through its input clamp diodes. Drive them LOW and latch for
    // deep sleep so they source no current.
    static const gpio_num_t itcon_pins[] = {
        BOARD_HAL_EPD_CS_PIN,
        BOARD_HAL_SPI_SCLK_PIN,
        BOARD_HAL_SPI_MOSI_PIN,
        BOARD_HAL_EPD_RST_PIN,
    };
    for (size_t i = 0; i < sizeof(itcon_pins) / sizeof(itcon_pins[0]); i++) {
        gpio_set_direction(itcon_pins[i], GPIO_MODE_OUTPUT);
        gpio_set_level(itcon_pins[i], 0);
        gpio_hold_en(itcon_pins[i]);
    }

    // Latch the battery load-switch enable LOW so it can't float and drain the
    // divider in deep sleep (matches EE02/EE04).
    gpio_set_direction(VBAT_ADC_ENABLE_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(VBAT_ADC_ENABLE_PIN, 0);
    gpio_hold_en(VBAT_ADC_ENABLE_PIN);
    // Latch the LED OFF through deep sleep so the pad can't float and glow.
    gpio_hold_en(BOARD_HAL_LED_PIN);
    // Latch PWR_EN LOW so the peripheral domain stays off in deep sleep.
    gpio_hold_en(BOARD_HAL_PWR_EN_PIN);
    gpio_deep_sleep_hold_en();

    // Release the ADC + calibration to save power.
    battery_adc_destroy(vbat_adc);
    vbat_adc = NULL;

    return ESP_OK;
}

bool board_hal_is_battery_connected(void)
{
    return board_hal_get_battery_voltage() > 2500;
}

int board_hal_get_battery_voltage(void)
{
    if (!vbat_adc)
        return -1;
    return battery_adc_read_mv(vbat_adc);
}

int board_hal_get_battery_percent(void)
{
    int voltage = board_hal_get_battery_voltage();
    if (voltage < 0)
        return -1;

    // Generic LiPo discharge curve (same mapping as the E1003; recalibrate with
    // VBAT_CAL_SCALE and this table if your pack reads off).
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
    // No readable charge-status GPIO on this board; use USB presence as a proxy.
    // Stays true while plugged in even after charge completes, and reads false
    // on data-less wall adapters (USB-serial-JTAG detection needs a USB host).
    return usb_serial_jtag_is_connected();
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
    return ESP_ERR_NOT_SUPPORTED;  // no external RTC on the EE03
}

esp_err_t board_hal_rtc_get_time(time_t *t)
{
    (void) t;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t board_hal_rtc_set_time(time_t t)
{
    (void) t;
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

void board_hal_led_set(board_hal_led_t led, bool on)
{
    // Single user LED (GPIO21, active-low); both POWER and ACTIVITY map to it.
    (void) led;
    gpio_set_level(BOARD_HAL_LED_PIN, BOARD_HAL_LED_INVERTED ? !on : on);
}
