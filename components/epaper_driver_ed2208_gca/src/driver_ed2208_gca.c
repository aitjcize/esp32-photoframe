#include <assert.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "epaper.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#ifdef CONFIG_PM_ENABLE
#include "esp_pm.h"
#endif

static const char *TAG = "epaper_ed2208_gca";

static epaper_config_t g_cfg;
static spi_device_handle_t spi;

#ifdef CONFIG_PM_ENABLE
static esp_pm_lock_handle_t pm_lock = NULL;
#endif

#define EPD_WIDTH 800
#define EPD_HEIGHT 480
// Packed pixel buffer size: 2 pixels per byte (4-bit color depth)
#define EPD_BUF_SIZE (EPD_WIDTH / 2 * EPD_HEIGHT)

// SPI max transfer size per transaction
#define SPI_MAX_CHUNK 4092
// Data transfer chunk size (per CS window)
#define DATA_CHUNK_SIZE 128

// --- Low-level SPI helpers ---

static void spi_begin(void)
{
    esp_err_t ret = spi_device_acquire_bus(spi, portMAX_DELAY);
    assert(ret == ESP_OK);
}

static void spi_end(void)
{
    spi_device_release_bus(spi);
}

static void spi_write(const uint8_t *data, size_t len)
{
    spi_transaction_t t = {};
    t.rxlength = 0;
    while (len > 0) {
        size_t chunk = (len > SPI_MAX_CHUNK) ? SPI_MAX_CHUNK : len;
        t.length = chunk * 8;
        t.tx_buffer = data;
        esp_err_t ret = spi_device_polling_start(spi, &t, portMAX_DELAY);
        if (ret == ESP_OK) {
            ret = spi_device_polling_end(spi, portMAX_DELAY);
        }
        assert(ret == ESP_OK);
        data += chunk;
        len -= chunk;
    }
}

// --- Display protocol helpers ---

// Send a command with optional data bytes in a single CS window.
// CS stays LOW for the entire command+data sequence.
static void cmd_data(uint8_t cmd, const uint8_t *data, size_t len)
{
    gpio_set_level(g_cfg.pin_dc, 0);  // DC low = command
    spi_begin();
    gpio_set_level(g_cfg.pin_cs, 0);  // CS low

    // Send command byte via SPI command register
    spi_transaction_ext_t cmd_t = {
        .command_bits = 8,
        .base =
            {
                .flags = SPI_TRANS_VARIABLE_CMD,
                .cmd = cmd,
            },
    };
    esp_err_t ret = spi_device_polling_start(spi, &cmd_t.base, portMAX_DELAY);
    if (ret == ESP_OK) {
        spi_device_polling_end(spi, portMAX_DELAY);
    }
    assert(ret == ESP_OK);

    if (len > 0) {
        gpio_set_level(g_cfg.pin_dc, 1);  // DC high = data
        // Copy to stack buffer to avoid PSRAM DMA issues
        uint8_t buf[16];
        assert(len <= sizeof(buf));
        memcpy(buf, data, len);
        spi_write(buf, len);
    }

    gpio_set_level(g_cfg.pin_cs, 1);  // CS high
    spi_end();
}

// Send a standalone command (no data bytes)
static void send_command(uint8_t cmd)
{
    cmd_data(cmd, NULL, 0);
}

// Send image buffer in DATA_CHUNK_SIZE-byte chunks, each in its own CS window,
// copied to a stack-local buffer to avoid PSRAM DMA issues.
static void send_buffer(uint8_t *data, int len)
{
    uint8_t buf[DATA_CHUNK_SIZE];
    uint8_t *ptr = data;
    int remaining = len;

    ESP_LOGI(TAG, "Sending %d bytes in %d-byte chunks", len, DATA_CHUNK_SIZE);

    while (remaining > 0) {
        int chunk = (remaining > DATA_CHUNK_SIZE) ? DATA_CHUNK_SIZE : remaining;

        // Copy to stack buffer (internal RAM) for reliable DMA
        memcpy(buf, ptr, chunk);

        gpio_set_level(g_cfg.pin_dc, 1);  // DC high = data
        spi_begin();
        gpio_set_level(g_cfg.pin_cs, 0);  // CS low
        spi_write(buf, chunk);
        gpio_set_level(g_cfg.pin_cs, 1);  // CS high
        spi_end();

        ptr += chunk;
        remaining -= chunk;
    }

    ESP_LOGI(TAG, "Buffer send complete");
}

static bool is_busy(void)
{
    int level = gpio_get_level(g_cfg.pin_busy);
    return level == 0;
}

static void wait_busy(const char *label)
{
    vTaskDelay(pdMS_TO_TICKS(10));
    int wait_count = 0;
    while (is_busy()) {
        vTaskDelay(pdMS_TO_TICKS(10));
        if (++wait_count > 4000) {  // 40s timeout
            ESP_LOGW(TAG, "[%s] BUSY timeout after 40s", label);
            return;
        }
    }
}

// --- Hardware setup ---

static void gpio_init(void)
{
    // Set desired output levels BEFORE enabling output drivers to avoid glitches
    gpio_set_level(g_cfg.pin_cs, 1);   // CS HIGH = deselected
    gpio_set_level(g_cfg.pin_dc, 0);   // DC LOW = command mode
    gpio_set_level(g_cfg.pin_rst, 1);  // RST HIGH = not in reset

    gpio_config_t out_conf = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << g_cfg.pin_rst) | (1ULL << g_cfg.pin_dc) | (1ULL << g_cfg.pin_cs),
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&out_conf));

    gpio_config_t in_conf = {
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << g_cfg.pin_busy),
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&in_conf));

    if (g_cfg.pin_enable >= 0) {
        // Release any pad hold latched by a previous deep-sleep cycle
        // (see epaper_enter_deepsleep) so gpio_config + gpio_set_level
        // below can re-drive the enable pin.
        gpio_hold_dis(g_cfg.pin_enable);
        gpio_config_t en_conf = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = (1ULL << g_cfg.pin_enable),
        };
        ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&en_conf));
        gpio_set_level(g_cfg.pin_enable, 1);
        vTaskDelay(pdMS_TO_TICKS(100));  // allow display power to stabilize
    }
}

static void spi_add_device(void)
{
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 20 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = -1,  // CS is manually controlled
        .queue_size = 1,
        .flags = SPI_DEVICE_HALFDUPLEX | SPI_DEVICE_NO_DUMMY,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(g_cfg.spi_host, &devcfg, &spi));
}

static void hw_reset(void)
{
    gpio_set_level(g_cfg.pin_rst, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(g_cfg.pin_rst, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(g_cfg.pin_rst, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
}

// --- Display operations ---

static void send_init_sequence(void)
{
    cmd_data(0xAA, (uint8_t[]){0x49, 0x55, 0x20, 0x08, 0x09, 0x18}, 6);  // CMDH
    cmd_data(0x01, (uint8_t[]){0x3F}, 1);                                // PWRR
    cmd_data(0x00, (uint8_t[]){0x5F, 0x69}, 2);                          // PSR
    cmd_data(0x03, (uint8_t[]){0x00, 0x54, 0x00, 0x44}, 4);              // POFS
    cmd_data(0x05, (uint8_t[]){0x40, 0x1F, 0x1F, 0x2C}, 4);              // BTST1
    cmd_data(0x06, (uint8_t[]){0x6F, 0x1F, 0x17, 0x49}, 4);              // BTST2
    cmd_data(0x08, (uint8_t[]){0x6F, 0x1F, 0x1F, 0x22}, 4);              // BTST3
    cmd_data(0x30, (uint8_t[]){0x03}, 1);                                // PLL
    cmd_data(0x50, (uint8_t[]){0x3F}, 1);                                // CDI
    cmd_data(0x60, (uint8_t[]){0x02, 0x00}, 2);                          // TCON
    cmd_data(0x61, (uint8_t[]){0x03, 0x20, 0x01, 0xE0}, 4);              // TRES
    cmd_data(0x84, (uint8_t[]){0x01}, 1);                                // T_VDCS
    cmd_data(0xE3, (uint8_t[]){0x2F}, 1);                                // PWS
}

// Full display update cycle:
// RESET -> INIT -> wait -> DTM -> DATA -> PON -> wait -> DRF -> wait -> POF -> wait -> DSLP
static void display_update_cycle(uint8_t *image)
{
#ifdef CONFIG_PM_ENABLE
    if (pm_lock) {
        esp_pm_lock_acquire(pm_lock);
    }
#endif

    hw_reset();
    wait_busy("reset");

    send_init_sequence();
    wait_busy("init");

    send_command(0x10);  // DATA_START_TRANSMISSION
    send_buffer(image, EPD_BUF_SIZE);
    wait_busy("data");

    send_command(0x04);  // POWER_ON
    wait_busy("power_on");

    cmd_data(0x12, (uint8_t[]){0x00}, 1);  // DISPLAY_REFRESH
    wait_busy("refresh");

    cmd_data(0x02, (uint8_t[]){0x00}, 1);  // POWER_OFF
    wait_busy("power_off");

    cmd_data(0x07, (uint8_t[]){0xA5}, 1);  // DEEP_SLEEP

#ifdef CONFIG_PM_ENABLE
    if (pm_lock) {
        esp_pm_lock_release(pm_lock);
    }
#endif
}

// --- Public API ---

uint16_t epaper_get_width(void)
{
    return EPD_WIDTH;
}

uint16_t epaper_get_height(void)
{
    return EPD_HEIGHT;
}

void epaper_init(const epaper_config_t *cfg)
{
    g_cfg = *cfg;

    ESP_LOGI(TAG, "Initializing ED2208-GCA (Spectra 6) E-Paper Driver");

    spi_add_device();
    gpio_init();

#ifdef CONFIG_PM_ENABLE
    esp_err_t ret = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "epd_update", &pm_lock);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create PM lock: %s", esp_err_to_name(ret));
    }
#endif
}

void epaper_clear(uint8_t *image, uint8_t color)
{
    uint8_t packed = (color << 4) | color;
    memset(image, packed, EPD_BUF_SIZE);

    ESP_LOGI(TAG, "Clearing display with color 0x%02x", color);
    display_update_cycle(image);
    ESP_LOGI(TAG, "Clear complete");
}

void epaper_display(uint8_t *image)
{
    ESP_LOGI(TAG, "Starting display update: %d bytes", EPD_BUF_SIZE);
    display_update_cycle(image);
    ESP_LOGI(TAG, "Display update complete");
}

void epaper_enter_deepsleep(void)
{
    ESP_LOGI(TAG, "Entering deep sleep");

#ifdef CONFIG_PM_ENABLE
    if (pm_lock) {
        esp_pm_lock_acquire(pm_lock);
    }
#endif

    // display_update_cycle() already sends POF + DSLP after each update,
    // so the display should already be in deep sleep. Send again to be safe.
    cmd_data(0x02, (uint8_t[]){0x00}, 1);  // POWER_OFF
    wait_busy("deepsleep_power_off");
    cmd_data(0x07, (uint8_t[]){0xA5}, 1);  // DEEP_SLEEP

    if (g_cfg.pin_enable >= 0) {
        vTaskDelay(pdMS_TO_TICKS(100));       // Ensure display enters sleep before cutting power
        gpio_set_level(g_cfg.pin_enable, 0);  // Cut power
        // Latch the pad low so the rail stays cut once the digital IO
        // domain powers down during deep sleep, and arm deep-sleep hold
        // so the latch survives into deep sleep. Both calls are
        // idempotent; other pins the board holds aren't affected.
        gpio_hold_en(g_cfg.pin_enable);
        gpio_deep_sleep_hold_en();
    }

#ifdef CONFIG_PM_ENABLE
    if (pm_lock) {
        esp_pm_lock_release(pm_lock);
    }
#endif
}
