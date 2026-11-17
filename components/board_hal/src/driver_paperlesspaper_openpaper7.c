#include "battery_adc.h"
#include "board_hal.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "epaper.h"
#include "esp_flash.h"
#include "esp_flash_spi_init.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board_hal_openpaper7";

// Label the storage code mounts LittleFS from (LITTLEFS_PARTITION_LABEL in
// main/storage.h). The partition table has no such entry on this board: the
// whole external flash is registered under this label instead.
#define EXT_STORAGE_PARTITION_LABEL "storage"

// FUSB303 USB-C port controller. STATUS bit 3 is VBUSOK; the vendor firmware
// also requires bit 6 clear before treating USB as present.
#define FUSB303_ADDR 0x21
#define FUSB303_REG_STATUS 0x11
#define FUSB303_STATUS_VBUSOK (1 << 3)
#define FUSB303_STATUS_BIT6 (1 << 6)

// Battery divider: 510k over 392k
#define VBAT_VOLTAGE_DIVIDER ((510.0f + 392.0f) / 392.0f)

// Optional per-unit correction for resistor-divider tolerance, measured with a
// multimeter: set to (multimeter_mV / firmware_reported_mV). 1.0 = none.
#ifndef VBAT_CAL_SCALE
#define VBAT_CAL_SCALE 1.0f
#endif

static i2c_master_bus_handle_t i2c_bus = NULL;
static i2c_master_dev_handle_t fusb303 = NULL;
static esp_flash_t *ext_flash = NULL;
static battery_adc_t *vbat_adc = NULL;
static int charger_enabled = -1;  // unknown until first set

// Let the charger run (CHG_EN floating) or stop it (CHG_EN low). Like the
// vendor firmware, charging is enabled only while USB power is present.
static void charger_enable(bool enable)
{
    if (charger_enabled == (int) enable) {
        return;
    }
    gpio_hold_dis(BOARD_HAL_CHG_EN_PIN);
    if (enable) {
        gpio_set_direction(BOARD_HAL_CHG_EN_PIN, GPIO_MODE_INPUT);
        gpio_set_pull_mode(BOARD_HAL_CHG_EN_PIN, GPIO_FLOATING);
    } else {
        gpio_set_level(BOARD_HAL_CHG_EN_PIN, 0);
        gpio_set_direction(BOARD_HAL_CHG_EN_PIN, GPIO_MODE_OUTPUT);
    }
    charger_enabled = enable;
    ESP_LOGI(TAG, "Charger %s", enable ? "enabled" : "disabled");
}

static void board_hal_battery_adc_init(void)
{
    if (vbat_adc)
        return;

    battery_adc_config_t cfg = {
        .unit = ADC_UNIT_1,
        .channel = BOARD_HAL_BAT_ADC_CHANNEL,
        .atten = ADC_ATTEN_DB_12,
        .enable_pin = -1,
        .settle_ms = 0,
        .samples = 8,
        .divider = VBAT_VOLTAGE_DIVIDER,
        .cal_scale = VBAT_CAL_SCALE,
    };
    battery_adc_create(&cfg, &vbat_adc);
}

// Probe the W25Q128 on the shared SPI bus and expose all of it as the storage
// partition, so the LittleFS code finds it by label like an internal one.
static void ext_flash_init(void)
{
    const esp_flash_spi_device_config_t dev_cfg = {
        .host_id = SPI2_HOST,
        .cs_io_num = BOARD_HAL_EXT_FLASH_CS_PIN,
        // Only DI/DO are routed; /WP and /HOLD are pulled up on the board
        .io_mode = SPI_FLASH_FASTRD,
        .freq_mhz = 20,
    };

    esp_err_t ret = spi_bus_add_flash_device(&ext_flash, &dev_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add external flash device: %s", esp_err_to_name(ret));
        ext_flash = NULL;
        return;
    }

    ret = esp_flash_init(ext_flash);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "External flash not responding: %s", esp_err_to_name(ret));
        spi_bus_remove_flash_device(ext_flash);
        ext_flash = NULL;
        return;
    }

    uint32_t id = 0;
    uint32_t size = 0;
    esp_flash_read_id(ext_flash, &id);
    esp_flash_get_size(ext_flash, &size);
    ESP_LOGI(TAG, "External flash: id 0x%06lx, %lu KB", (unsigned long) id,
             (unsigned long) (size / 1024));

    const esp_partition_t *part = NULL;
    ret = esp_partition_register_external(ext_flash, 0, size, EXT_STORAGE_PARTITION_LABEL,
                                          ESP_PARTITION_TYPE_DATA,
                                          ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, &part);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register external storage partition: %s", esp_err_to_name(ret));
    }
}

esp_err_t board_hal_init(void)
{
    ESP_LOGI(TAG, "Initializing paperlesspaper OpenPaper 7 Board HAL");

    // Release pad holds latched by the previous deep-sleep cycle so these
    // outputs can be reconfigured; board_hal_prepare_for_sleep() re-applies them.
    gpio_hold_dis(BOARD_HAL_LED_PIN);
    gpio_hold_dis(BOARD_HAL_CHG_EN_PIN);

    // --- Charger status (CHG_EN is set once USB presence is known, below) ---
    gpio_reset_pin(BOARD_HAL_CHG_EN_PIN);
    gpio_config_t chg_stat_cfg = {
        .pin_bit_mask = (1ULL << BOARD_HAL_CHG_STAT_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&chg_stat_cfg);

    // --- SPI bus (panel + external flash) ---
    // Deselect both devices before the bus starts toggling
    gpio_config_t cs_cfg = {
        .pin_bit_mask = (1ULL << BOARD_HAL_EPD_CS_PIN) | (1ULL << BOARD_HAL_EXT_FLASH_CS_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&cs_cfg);
    gpio_set_level(BOARD_HAL_EPD_CS_PIN, 1);
    gpio_set_level(BOARD_HAL_EXT_FLASH_CS_PIN, 1);

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
        // The vendor firmware drives this panel with Good Display's
        // GDEP073E01 register set rather than the Waveshare/Seeed one
        .gca_gdep073e01_init = true,
    };
    epaper_init(&ep_cfg);

    // --- External flash (photo storage) ---
    ext_flash_init();

    // --- Battery ADC ---
    board_hal_battery_adc_init();

    // --- Onboard LED ---
    gpio_config_t led_cfg = {
        .pin_bit_mask = (1ULL << BOARD_HAL_LED_PIN),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&led_cfg);
    board_hal_led_set(BOARD_HAL_LED_ACTIVITY, false);

    // --- I2C bus (FUSB303 for USB detection) ---
    i2c_master_bus_config_t i2c_bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = 0,
        .scl_io_num = BOARD_HAL_I2C_SCL_PIN,
        .sda_io_num = BOARD_HAL_I2C_SDA_PIN,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&i2c_bus_config, &i2c_bus) == ESP_OK) {
        if (i2c_master_probe(i2c_bus, FUSB303_ADDR, 100) == ESP_OK) {
            i2c_device_config_t dev_cfg = {
                .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                .device_address = FUSB303_ADDR,
                .scl_speed_hz = 100000,
            };
            if (i2c_master_bus_add_device(i2c_bus, &dev_cfg, &fusb303) == ESP_OK) {
                ESP_LOGI(TAG, "FUSB303 USB-C controller found");
            }
        } else {
            ESP_LOGW(TAG, "FUSB303 not found; USB detection falls back to charger status");
        }
    } else {
        ESP_LOGE(TAG, "Failed to initialize I2C bus");
    }

    // Sets the charger to match: on with USB power, off on battery
    ESP_LOGI(TAG, "USB power %s", board_hal_is_usb_connected() ? "present" : "absent");

    return ESP_OK;
}

esp_err_t board_hal_prepare_for_sleep(void)
{
    ESP_LOGI(TAG, "Preparing OpenPaper 7 for sleep");

    board_hal_led_set(BOARD_HAL_LED_ACTIVITY, false);

    epaper_enter_deepsleep();

    // The external flash stays on the bus: storage is unmounted after this
    // returns (power_manager_enter_sleep), and the W25Q128 idles at a few uA.

    battery_adc_destroy(vbat_adc);
    vbat_adc = NULL;

    // Keep the LED off, and the charger as it is (charging on USB, stopped on
    // battery), through deep sleep. The C6 holds single pads in deep sleep, so
    // no gpio_deep_sleep_hold_en() is needed. A USB cable plugged in during
    // sleep starts charging at the next wake.
    gpio_hold_en(BOARD_HAL_LED_PIN);
    gpio_hold_en(BOARD_HAL_CHG_EN_PIN);

    return ESP_OK;
}

bool board_hal_is_battery_connected(void)
{
    // The ADC reads the battery rail directly; on USB with no cells fitted it
    // sits near zero.
    return board_hal_get_battery_voltage() > 2000;
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

    // Four NiMH cells. The curve is flat through the middle, so this is coarse;
    // 4.2V (1.05V/cell) is where the vendor firmware stops running.
    static const struct {
        int mv;
        int pct;
    } cal[] = {
        {5500, 100}, {5300, 90}, {5150, 75}, {5050, 60}, {5000, 50}, {4920, 40},
        {4840, 30},  {4760, 20}, {4600, 10}, {4400, 5},  {4200, 0},
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
    return gpio_get_level(BOARD_HAL_CHG_STAT_PIN) == 0;
}

bool board_hal_is_usb_connected(void)
{
    if (fusb303) {
        uint8_t reg = FUSB303_REG_STATUS;
        uint8_t status = 0;
        if (i2c_master_transmit_receive(fusb303, &reg, 1, &status, 1, 100) == ESP_OK) {
            bool usb = (status & FUSB303_STATUS_VBUSOK) && !(status & FUSB303_STATUS_BIT6);
            // Polled every second while awake, so the charger follows the cable
            charger_enable(usb);
            return usb;
        }
    }
    // Without the FUSB303 there is no telling USB power apart, so leave the
    // charger on (it only runs from USB power) and go by its status
    charger_enable(true);
    return board_hal_is_charging();
}

void board_hal_shutdown(void)
{
    ESP_LOGI(TAG, "Shutdown requested, entering deep sleep");
    board_hal_prepare_for_sleep();
    esp_deep_sleep_start();
}

esp_err_t board_hal_rtc_init(void)
{
    // No external RTC; time comes from SNTP and the internal RTC
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

void board_hal_led_set(board_hal_led_t led, bool on)
{
    // Single LED, used for both status and activity
    (void) led;
    gpio_set_level(BOARD_HAL_LED_PIN, on);
}

esp_err_t board_hal_get_temperature(float *t)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t board_hal_get_humidity(float *h)
{
    return ESP_ERR_NOT_SUPPORTED;
}
