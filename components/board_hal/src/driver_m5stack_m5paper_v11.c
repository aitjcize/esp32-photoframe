// M5Stack M5Paper v1.1 board HAL.
//
// ESP32-D0WDQ6-V3 (not an S3) + 8MB quad PSRAM driving a 4.7" 960x540 16-level
// grayscale panel through an IT8951 T-CON on SPI, with a microSD card on the
// same bus. Peripherals: SHT30 (0x44) and BM8563 RTC (0x51) on I2C0, battery
// voltage on GPIO35 behind a fixed 2:1 divider. v1.0 is electrically identical
// — only the panel changed from rigid to flexible — so it uses this driver too.
//
// Two things are specific to this board and easy to get wrong:
//
//   * Main power latch (GPIO2). The side power button only pulses the regulator
//     on; firmware must drive GPIO2 high or the board switches itself off a
//     moment after boot whenever it is running from the battery. It is the first
//     thing this driver does, and it is latched through deep sleep.
//
//   * No IT8951 reset line. M5Stack resets the controller by cycling its supply
//     (EPD_PWR_EN, GPIO23) instead, so epaper_init() is given pin_rst = -1 and
//     skips its reset pulse.

#include <math.h>

#include "battery_adc.h"
#include "board_hal.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/rtc_io.h"
#include "driver/spi_master.h"
#include "epaper.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pcf8563.h"
#include "sensor.h"

#ifdef CONFIG_HAS_SDCARD
#include "sdcard.h"
#endif

static const char *TAG = "board_hal_m5paper_v11";

static i2c_master_bus_handle_t i2c_bus = NULL;

// Battery measurement constants
#define VBAT_ADC_CHANNEL BOARD_HAL_BAT_ADC_PIN
// Voltage divider ratio: 2.0, matching M5Stack's own SCALE_INV.
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

    // GPIO35 (ADC1_CH7) behind a permanently-connected 2:1 divider — there is no
    // load switch to gate, so enable_pin is -1 and no settling delay is needed.
    battery_adc_config_t cfg = {
        .unit = ADC_UNIT_1,
        .channel = VBAT_ADC_CHANNEL,
        .atten = ADC_ATTEN_DB_12,
        .enable_pin = BOARD_HAL_BAT_EN_PIN,
        .settle_ms = 0,
        .samples = 8,
        .divider = VBAT_VOLTAGE_DIVIDER,
        .cal_scale = VBAT_CAL_SCALE,
    };
    battery_adc_create(&cfg, &vbat_adc);
}

// Drive the main-power latch high and keep it there. Called before anything
// else: on battery this is what stops the board powering itself back off.
//
// GPIO2 is one of the ESP32's RTC pads, and it is driven through the RTC IO mux
// rather than the digital one so rtc_gpio_hold_en() can latch it across deep
// sleep. Latching it through the digital mux would not survive: the digital pad
// registers lose power in deep sleep, and this pin dropping low powers the whole
// board off, leaving only the side button able to revive it — a timer wake would
// never happen.
static void main_power_hold(void)
{
    // Configure and drive the pad high while last cycle's hold still freezes
    // it, and release the hold last. The RTC output register does keep its
    // value through sleep, but on a pin that powers the whole board off if it
    // ever reads low, don't hand over with the output unconfigured.
    rtc_gpio_init(BOARD_HAL_MAIN_PWR_PIN);
    rtc_gpio_set_direction(BOARD_HAL_MAIN_PWR_PIN, RTC_GPIO_MODE_OUTPUT_ONLY);
    rtc_gpio_set_level(BOARD_HAL_MAIN_PWR_PIN, 1);
    rtc_gpio_hold_dis(BOARD_HAL_MAIN_PWR_PIN);
}

esp_err_t board_hal_init(void)
{
    ESP_LOGI(TAG, "Initializing M5Paper v1.1 Board HAL");

    main_power_hold();

    // Release the remaining pad holds latched by the previous deep-sleep cycle.
    gpio_hold_dis(BOARD_HAL_EPD_ENABLE_PIN);
    gpio_hold_dis(BOARD_HAL_EXT_PWR_EN_PIN);

    // prepare_for_sleep() parks SPI MOSI (GPIO12) with rtc_gpio_isolate(), which
    // switches the pad to the RTC mux, disables its driver and holds it. Those
    // settings live in the RTC domain and survive deep sleep, so they have to be
    // undone here or the SPI bus would come up with a dead MOSI on every wake
    // after the first — hand the pad back to the digital mux before the bus is
    // initialized below.
    rtc_gpio_hold_dis(BOARD_HAL_SPI_MOSI_PIN);
    rtc_gpio_deinit(BOARD_HAL_SPI_MOSI_PIN);

    // --- Power rails ---
    // EPD_PWR_EN feeds the IT8951 and the panel bias. EXT_PWR_EN feeds the Grove
    // ports only; keep it off so it isn't a standing drain.
    gpio_config_t rail_cfg = {
        .pin_bit_mask = (1ULL << BOARD_HAL_EPD_ENABLE_PIN) | (1ULL << BOARD_HAL_EXT_PWR_EN_PIN),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&rail_cfg);
    gpio_set_level(BOARD_HAL_EXT_PWR_EN_PIN, 0);
    gpio_set_level(BOARD_HAL_EPD_ENABLE_PIN, 1);
    // The IT8951 needs its rail settled before it answers on SPI. M5Stack's own
    // driver waits a full second here; the controller returns garbage device
    // info if this is cut short, and there is no reset line to recover with.
    vTaskDelay(pdMS_TO_TICKS(1000));

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
        // Matches the E1003: the IT8951 driver streams image data in <=4000-byte
        // bursts, and a small DMA buffer keeps SD sector-read DMA on the shared
        // bus unaffected.
        .max_transfer_sz = 4092,
        .flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_SCLK,
    };
    // HSPI (SPI2_HOST). MOSI/MISO are swapped relative to HSPI's IOMUX pins
    // (IOMUX wants MOSI on 13 and MISO on 12), so the bus routes through the
    // GPIO matrix. That caps the usable clock well below IOMUX speeds, which
    // costs nothing here: the IT8951 runs at 4 MHz regardless.
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    // --- E-Paper Display (IT8951) ---
    epaper_config_t ep_cfg = {
        .spi_host = SPI2_HOST,
        .pin_cs = BOARD_HAL_EPD_CS_PIN,
        .pin_dc = BOARD_HAL_EPD_DC_PIN,      // unused on IT8951
        .pin_rst = BOARD_HAL_EPD_RST_PIN,    // -1: reset is done by cycling the rail
        .pin_busy = BOARD_HAL_EPD_BUSY_PIN,  // HRDY
        .pin_cs1 = BOARD_HAL_EPD_CS1_PIN,    // unused
        // The rail is already up (and had its settling delay); passing -1 keeps
        // the driver from re-toggling it underneath a controller that is
        // already running.
        .pin_enable = -1,
        .panel_w = BOARD_HAL_EPD_PANEL_WIDTH,
        .panel_h = BOARD_HAL_EPD_PANEL_HEIGHT,
        .vcom_mv = BOARD_HAL_EPD_VCOM_MV,
        // M5Stack's driver loads 4bpp data big-endian and writes rows straight
        // through — this panel scans left-to-right, unlike the Seeed ED103TC2.
        .big_endian = true,
        .mirror_x = false,
    };
    epaper_init(&ep_cfg);

    // --- SD Card ---
#ifdef CONFIG_HAS_SDCARD
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
    board_hal_battery_adc_init();

    // --- I2C bus (RTC + sensor on I2C0) ---
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
            ESP_LOGI(TAG, "SHT30 sensor initialized");
            // Feed the live panel temperature to the IT8951 so it selects the
            // right grayscale waveform (better tone, less ghosting than a fixed
            // default). Re-read each wake, so deep-sleep cycles stay accurate.
            float temp_c = 0.0f, humidity = 0.0f;
            if (sensor_read(&temp_c, &humidity) == ESP_OK) {
                epaper_set_temperature((int8_t) lroundf(temp_c));
                ESP_LOGI(TAG, "Panel temperature set to %.1f C", temp_c);
            }
        }
        // The BM8563 is a PCF8563 clone and shares its register map.
        if (pcf8563_init(i2c_bus) == ESP_OK) {
            ESP_LOGI(TAG, "BM8563 RTC initialized");
        }
    } else {
        ESP_LOGE(TAG, "Failed to initialize I2C bus: %s", esp_err_to_name(i2c_ret));
    }

    return ESP_OK;
}

esp_err_t board_hal_prepare_for_sleep(void)
{
    ESP_LOGI(TAG, "Preparing M5Paper v1.1 for sleep");

    if (sensor_is_available()) {
        sensor_sleep();
        ESP_LOGI(TAG, "SHT30 sensor put to sleep");
    }

    // Put the IT8951 to sleep, then cut the rail that feeds it and the panel
    // bias. On this board that is also the controller's only reset, so the next
    // wake comes up with a freshly initialized IT8951.
    epaper_enter_deepsleep();
    gpio_set_level(BOARD_HAL_EPD_ENABLE_PIN, 0);

#ifdef CONFIG_HAS_SDCARD
    sdcard_deinit();
#endif

    // GPIO12 — SPI MOSI on this board — is also the ESP32's MTDI strapping pin,
    // which selects the flash voltage at every reset, including the one that
    // ends deep sleep. Left driven or pulled high it can bring the chip back up
    // with VDD_SDIO at 1.8V, and it then fails to boot. Isolating the pad (the
    // workaround ESP-IDF's own deep-sleep example uses) lets it settle low, so
    // the wake reset reads the 3.3V strapping.
    rtc_gpio_isolate(BOARD_HAL_SPI_MOSI_PIN);

    battery_adc_destroy(vbat_adc);
    vbat_adc = NULL;

    // Latch output levels through deep sleep. MAIN_PWR above all: if it drops,
    // the board powers off completely and only the side power button will bring
    // it back — a timer wake would never happen. It is an RTC pad, so it gets
    // the RTC latch (which stays powered in deep sleep); the EPD and Grove rail
    // pins are ordinary digital pads held by gpio_deep_sleep_hold_en().
    rtc_gpio_set_level(BOARD_HAL_MAIN_PWR_PIN, 1);
    rtc_gpio_hold_en(BOARD_HAL_MAIN_PWR_PIN);
    gpio_hold_en(BOARD_HAL_EPD_ENABLE_PIN);
    gpio_hold_en(BOARD_HAL_EXT_PWR_EN_PIN);
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

    // Single-cell LiPo discharge curve (1150mAh cell, charged to 4.2V).
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
    // The charger (an IP5306-class part) exposes no status line to the ESP32 and
    // there is no I2C charger on this board, so charge state is not observable.
    return false;
}

bool board_hal_is_usb_connected(void)
{
    // No VBUS sense GPIO, no I2C charger to ask, and the ESP32 has no
    // USB-Serial-JTAG peripheral to infer a host connection from — USB power is
    // simply not detectable here. Users who want the device always reachable on
    // USB should disable deep sleep in Settings > General.
    return false;
}

void board_hal_shutdown(void)
{
    // TODO: a real shutdown on this board is dropping the GPIO2 latch, which
    // cuts the board's own power (the side button brings it back). Nothing in
    // main/ calls this yet, and it hasn't been tried on hardware, so for now
    // it sleeps with the latch held, like the other boards.
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
    // No user-controllable LED on this board.
    (void) led;
    (void) on;
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
