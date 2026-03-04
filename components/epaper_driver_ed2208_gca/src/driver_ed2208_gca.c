#include <assert.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "epaper.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "epaper_ed2208_gca";

static epaper_config_t g_cfg;
static spi_device_handle_t spi;

#define EPD_WIDTH 800
#define EPD_HEIGHT 480
// Packed pixel buffer size: 2 pixels per byte (4-bit color depth)
#define EPD_BUF_SIZE (EPD_WIDTH / 2 * EPD_HEIGHT)

// --- Low-level helpers ---

static void spi_send_byte(uint8_t data)
{
    spi_transaction_t t = {};
    t.length = 8;
    t.tx_buffer = &data;
    esp_err_t ret = spi_device_polling_transmit(spi, &t);
    assert(ret == ESP_OK);
}

static void send_command(uint8_t cmd)
{
    gpio_set_level(g_cfg.pin_dc, 0);  // DC low = command
    gpio_set_level(g_cfg.pin_cs, 0);
    spi_send_byte(cmd);
    gpio_set_level(g_cfg.pin_cs, 1);
}

static void send_data(uint8_t data)
{
    gpio_set_level(g_cfg.pin_dc, 1);  // DC high = data
    gpio_set_level(g_cfg.pin_cs, 0);
    spi_send_byte(data);
    gpio_set_level(g_cfg.pin_cs, 1);
}

static void send_buffer(uint8_t *data, int len)
{
    gpio_set_level(g_cfg.pin_dc, 1);  // DC high = data
    gpio_set_level(g_cfg.pin_cs, 0);  // Hold CS low for entire transfer

    spi_transaction_t t = {};
    int chunks = len / 5000;
    int remainder = len % 5000;
    uint8_t *ptr = data;

    ESP_LOGI(TAG, "Sending %d bytes in %d chunks of 5000 + %d remainder", len, chunks, remainder);

    int chunk = 0;
    while (chunks--) {
        t.length = 8 * 5000;
        t.tx_buffer = ptr;
        esp_err_t ret = spi_device_polling_transmit(spi, &t);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "SPI transmit failed at chunk %d: %s", chunk, esp_err_to_name(ret));
        }
        assert(ret == ESP_OK);
        ptr += 5000;
        chunk++;

        // Yield to watchdog every 10 chunks (~50KB)
        if (chunk % 10 == 0) {
            vTaskDelay(1);
        }
    }

    if (remainder > 0) {
        t.length = 8 * remainder;
        t.tx_buffer = ptr;
        esp_err_t ret = spi_device_polling_transmit(spi, &t);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "SPI transmit failed at final chunk: %s", esp_err_to_name(ret));
        }
        assert(ret == ESP_OK);
    }

    gpio_set_level(g_cfg.pin_cs, 1);
    ESP_LOGI(TAG, "Buffer send complete");
}

static void wait_busy(void)
{
    int wait_count = 0;
    while (!gpio_get_level(g_cfg.pin_busy)) {
        vTaskDelay(pdMS_TO_TICKS(10));
        if (++wait_count > 4000) {  // 40s timeout
            ESP_LOGW(TAG, "Display busy timeout after 40s");
            return;
        }
    }
}

// --- Hardware setup ---

static void gpio_init(void)
{
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

    gpio_set_level(g_cfg.pin_rst, 1);

    if (g_cfg.pin_enable >= 0) {
        gpio_config_t en_conf = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = (1ULL << g_cfg.pin_enable),
            .pull_up_en = GPIO_PULLUP_DISABLE,
        };
        ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&en_conf));
        gpio_set_level(g_cfg.pin_enable, 1);
        vTaskDelay(pdMS_TO_TICKS(100));  // allow display power to stabilize
    }
}

static void spi_add_device(void)
{
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = (g_cfg.spi_host == SPI3_HOST) ? 40 * 1000 * 1000 : 10 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = -1,  // CS is manually controlled
        .queue_size = 7,
        .flags = SPI_DEVICE_HALFDUPLEX,
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

static void turn_on_display(void)
{
    send_command(0x04);  // POWER_ON
    wait_busy();

    send_command(0x12);  // DISPLAY_REFRESH
    send_data(0x00);
    wait_busy();

    send_command(0x02);  // POWER_OFF
    send_data(0x00);
    wait_busy();
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
    hw_reset();
    wait_busy();
    vTaskDelay(pdMS_TO_TICKS(50));

    // CMDH (0xAA) - Command Header (unlock command access)
    send_command(0xAA);
    send_data(0x49);
    send_data(0x55);
    send_data(0x20);
    send_data(0x08);
    send_data(0x09);
    send_data(0x18);

    // PWRR (0x01) - Power Setting Register
    send_command(0x01);
    send_data(0x3F);
    send_data(0x00);
    send_data(0x32);
    send_data(0x2A);
    send_data(0x0E);
    send_data(0x2A);

    // PSR (0x00) - Panel Setting
    send_command(0x00);
    send_data(0x5F);
    send_data(0x69);

    // POFS (0x03) - Power OFF Sequence Setting
    send_command(0x03);
    send_data(0x00);
    send_data(0x54);
    send_data(0x00);
    send_data(0x44);

    // BTST1 (0x05) - Booster Soft Start 1
    send_command(0x05);
    send_data(0x40);
    send_data(0x1F);
    send_data(0x1F);
    send_data(0x2C);

    // BTST2 (0x06) - Booster Soft Start 2
    send_command(0x06);
    send_data(0x6F);
    send_data(0x1F);
    send_data(0x16);
    send_data(0x25);

    // BTST3 (0x08) - Booster Soft Start 3
    send_command(0x08);
    send_data(0x6F);
    send_data(0x1F);
    send_data(0x1F);
    send_data(0x22);

    // IPC (0x13) - Internal Power Control
    send_command(0x13);
    send_data(0x00);
    send_data(0x04);

    // PLL (0x30) - PLL Control
    send_command(0x30);
    send_data(0x02);

    // TSE (0x41) - Temperature Sensor Enable
    send_command(0x41);
    send_data(0x00);

    // CDI (0x50) - VCOM and Data Interval Setting
    send_command(0x50);
    send_data(0x3F);

    // TCON (0x60) - TCON Setting
    send_command(0x60);
    send_data(0x02);
    send_data(0x00);

    // TRES (0x61) - Resolution Setting (800 x 480)
    send_command(0x61);
    send_data(0x03);
    send_data(0x20);
    send_data(0x01);
    send_data(0xE0);

    // VDCS (0x82) - VCOM DC Setting
    send_command(0x82);
    send_data(0x1E);

    // T_VDCS (0x84) - Temperature VCOM DC Setting
    send_command(0x84);
    send_data(0x01);

    // AGID (0x86)
    send_command(0x86);
    send_data(0x00);

    // PWS (0xE3) - Power Width Setting
    send_command(0xE3);
    send_data(0x2F);

    // CCSET (0xE0) - Color Control Setting
    send_command(0xE0);
    send_data(0x00);

    // TSSET (0xE6) - Temperature Sensor Setting
    send_command(0xE6);
    send_data(0x00);

    // PON (0x04) - Power ON
    send_command(0x04);
    wait_busy();
}

void epaper_clear(uint8_t *image, uint8_t color)
{
    uint8_t packed = (color << 4) | color;
    memset(image, packed, EPD_BUF_SIZE);

    send_command(0x10);
    send_buffer(image, EPD_BUF_SIZE);
    turn_on_display();
}

void epaper_display(uint8_t *image)
{
    ESP_LOGI(TAG, "Starting display update: %d bytes", EPD_BUF_SIZE);

    send_command(0x10);
    send_buffer(image, EPD_BUF_SIZE);
    turn_on_display();

    ESP_LOGI(TAG, "Display update complete");
}

void epaper_enter_deepsleep(void)
{
    ESP_LOGI(TAG, "Entering deep sleep");

    // Power OFF
    send_command(0x02);
    send_data(0x00);
    wait_busy();

    // Deep Sleep
    send_command(0x07);
    send_data(0xA5);

    if (g_cfg.pin_enable >= 0) {
        vTaskDelay(pdMS_TO_TICKS(100));       // Ensure display enters sleep before cutting power
        gpio_set_level(g_cfg.pin_enable, 0);  // Cut power
    }
}
