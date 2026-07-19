#include "battery_adc.h"
#include "board_hal.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/usb_serial_jtag.h"
#include "epaper.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board_hal_ee04";

// Pin Definitions for XIAO EE04
// XIAO ESP32-S3 has precise battery voltage on IO1 (A0) via voltage divider (R11=100k, R10=100k =>
// factor 2)
#define VBAT_ADC_CHANNEL ADC_CHANNEL_0  // GPIO 1 is ADC1_CHANNEL_0
#define VBAT_VOLTAGE_DIVIDER 2.0f
#define VBAT_ADC_ENABLE_PIN GPIO_NUM_6  // TPS22916 enable - must be HIGH to read battery voltage

// Optional per-unit correction for resistor-divider tolerance, measured with a
// multimeter: set to (multimeter_mV / firmware_reported_mV). 1.0 = none.
#ifndef VBAT_CAL_SCALE
#define VBAT_CAL_SCALE 1.0f
#endif

static battery_adc_t *vbat_adc = NULL;

esp_err_t board_hal_init(void)
{
    ESP_LOGI(TAG, "Initializing XIAO EE04 Board HAL");

    // Release any pad hold latched by the previous deep-sleep cycle so we
    // can reconfigure the TPS22916 enable pin during init.
    gpio_hold_dis(VBAT_ADC_ENABLE_PIN);

    // Initialize SPI bus
    ESP_LOGI(TAG, "Initializing SPI bus...");
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = BOARD_HAL_SPI_MOSI_PIN,
        .miso_io_num = -1,
        .sclk_io_num = BOARD_HAL_SPI_SCLK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 800 * 480 / 2 + 100,  // Sufficient for 7.3" EPD
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    // Initialize E-Paper Display Port
    epaper_config_t ep_cfg = {
        .spi_host = SPI2_HOST,
        .pin_cs = BOARD_HAL_EPD_CS_PIN,
        .pin_dc = BOARD_HAL_EPD_DC_PIN,
        .pin_rst = BOARD_HAL_EPD_RST_PIN,
        .pin_busy = BOARD_HAL_EPD_BUSY_PIN,
        .pin_cs1 = -1,
        .pin_enable = BOARD_HAL_EPD_ENABLE_PIN,
        .btst2 = BOARD_HAL_EPD_BTST2,
    };
    epaper_init(&ep_cfg);

    // Battery voltage ADC (shared helper handles calibration + averaging).
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

    // Configure ADC enable pin (TPS22916 load switch)
    // GPIO6 must be HIGH before reading battery voltage on GPIO1 (A0).
    // The TPS22916 gates the voltage divider; without enabling it, ADC reads 0V.
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << VBAT_ADC_ENABLE_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(VBAT_ADC_ENABLE_PIN, 0);  // Keep LOW by default to save power

    return ESP_OK;
}

esp_err_t board_hal_prepare_for_sleep(void)
{
    ESP_LOGI(TAG, "Preparing EE04 for sleep");

    epaper_enter_deepsleep();

    // Drive TPS22916 enable LOW and latch the pad so the load switch stays
    // disabled once the digital IO domain powers down. Without the hold the
    // pin can float, re-enable the switch, and continuously drain the
    // 100k+100k battery divider.
    gpio_set_direction(VBAT_ADC_ENABLE_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(VBAT_ADC_ENABLE_PIN, 0);
    gpio_hold_en(VBAT_ADC_ENABLE_PIN);
    gpio_deep_sleep_hold_en();

    // Release the ADC + calibration to save power.
    battery_adc_destroy(vbat_adc);
    vbat_adc = NULL;

    return ESP_OK;
}

bool board_hal_is_battery_connected(void)
{
    int voltage = board_hal_get_battery_voltage();
    return voltage > 2500;  // If we read > 2.5V, a battery is connected
}

int board_hal_get_battery_voltage(void)
{
    return battery_adc_read_mv(vbat_adc);
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
    // On BQ24070 revisions (schematic <= v1.1) the STAT charge-status outputs
    // only drive the onboard LEDs — not routed to any XIAO GPIO — so the real
    // charge state cannot be read; USB presence is the best available proxy
    // (the charger charges whenever USB power is present, until done).
    // Limitations: stays true while plugged in even after the charge completes,
    // and reads false on data-less wall adapters (USB-serial-JTAG detection
    // needs a USB host).
    // TODO: schematic v1.2 (2025-10) replaced the charger with an SY6974B on
    // I2C — probing it (like the reTerminal e1003/e1004 drivers do) would give
    // the real charge state on those boards.
    return usb_serial_jtag_is_connected();
}

bool board_hal_is_usb_connected(void)
{
    return usb_serial_jtag_is_connected();
}

void board_hal_shutdown(void)
{
    ESP_LOGI(TAG, "Shutdown not supported on BQ24070, entering deep sleep instead");
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

void board_hal_led_set(board_hal_led_t led, bool on)
{
    // No onboard LED on XIAO EE04
    (void) led;
    (void) on;
}
