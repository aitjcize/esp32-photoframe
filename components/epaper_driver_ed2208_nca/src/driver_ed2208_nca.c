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

static const char *TAG = "epaper_ed2208_nca";

static epaper_config_t g_cfg;
static spi_device_handle_t spi;

#ifdef CONFIG_PM_ENABLE
static esp_pm_lock_handle_t pm_lock = NULL;
#endif

#define EPD_WIDTH 1200
#define EPD_HEIGHT 1600
// Packed pixel buffer size: 2 pixels per byte (4-bit color depth)
#define EPD_BUF_SIZE (EPD_WIDTH / 2 * EPD_HEIGHT)

// --- Initialization Data (from T133A01_Defines.h) ---
static const uint8_t PSR_V[] = {0xDF, 0x69};
static const uint8_t PWR_V[] = {0x0F, 0x00, 0x28, 0x2C, 0x28, 0x38};
static const uint8_t POF_V[] = {0x00};
static const uint8_t DRF_V[] = {0x01};
static const uint8_t CDI_V[] = {0x37};
static const uint8_t TRES_V[] = {0x04, 0xB0, 0x03, 0x20};
static const uint8_t CCSET_V_CUR[] = {0x01};
static const uint8_t PWS_V[] = {0x22};
static const uint8_t BTST_P_V[] = {0xD8, 0x18};
static const uint8_t BTST_N_V[] = {0xD8, 0x18};

static const uint8_t r74DataBuf[] = {0xC0, 0x1C, 0x1C, 0xCC, 0xCC, 0xCC, 0x15, 0x15, 0x55};
static const uint8_t rf0DataBuf[] = {0x49, 0x55, 0x13, 0x5D, 0x05, 0x10};
static const uint8_t r60DataBuf[] = {0x03, 0x03};
static const uint8_t r86DataBuf[] = {0x10};
static const uint8_t rb6DataBuf[] = {0x07};
static const uint8_t rb7DataBuf[] = {0x01};
static const uint8_t rb0DataBuf[] = {0x01};
static const uint8_t rb1DataBuf[] = {0x02};

// --- Low-level SPI helpers ---

// Send a command with optional data bytes in a single CS window.
// CS stays LOW for the entire command+data sequence.
static void cmd_data(uint8_t cmd, const uint8_t *data, size_t len)
{
    gpio_set_level(g_cfg.pin_cs, 0);  // CS low

    // Send Command
    gpio_set_level(g_cfg.pin_dc, 0);  // DC low = command
    spi_transaction_t t_cmd = {.length = 8, .tx_buffer = &cmd};
    spi_device_transmit(spi, &t_cmd);

    // Send Data
    if (len > 0) {
        gpio_set_level(g_cfg.pin_dc, 1);  // DC high = data
        spi_transaction_t t_data = {.length = len * 8, .tx_buffer = data};
        spi_device_transmit(spi, &t_data);
    }

    gpio_set_level(g_cfg.pin_cs, 1);  // CS high
}

// Send a command targeting both controllers (CS1 LOW during cmd_data)
static void cmd_data_both(uint8_t cmd, const uint8_t *data, size_t len)
{
    gpio_set_level(g_cfg.pin_cs1, 0);
    cmd_data(cmd, data, len);
    gpio_set_level(g_cfg.pin_cs1, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
}

static bool is_busy(void)
{
    return gpio_get_level(g_cfg.pin_busy) == 0;
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
    // Release any pad holds latched by a previous deep-sleep cycle
    // (see epaper_enter_deepsleep) so gpio_config + gpio_set_level
    // below can re-drive these pins.
    gpio_hold_dis(g_cfg.pin_cs);
    gpio_hold_dis(g_cfg.pin_cs1);
    gpio_hold_dis(g_cfg.pin_dc);
    gpio_hold_dis(g_cfg.pin_rst);

    // Set desired output levels BEFORE enabling output drivers to avoid glitches
    gpio_set_level(g_cfg.pin_cs, 1);   // CS HIGH = deselected
    gpio_set_level(g_cfg.pin_cs1, 1);  // CS1 HIGH = deselected
    gpio_set_level(g_cfg.pin_dc, 0);   // DC LOW = command mode
    gpio_set_level(g_cfg.pin_rst, 1);  // RST HIGH = not in reset

    gpio_config_t out_conf = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << g_cfg.pin_rst) | (1ULL << g_cfg.pin_dc) | (1ULL << g_cfg.pin_cs) |
                        (1ULL << g_cfg.pin_cs1),
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
        .clock_speed_hz = 10 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = -1,  // CS is manually controlled
        .queue_size = 7,
        .flags = SPI_DEVICE_HALFDUPLEX,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(g_cfg.spi_host, &devcfg, &spi));
}

static void hw_reset(void)
{
    gpio_set_level(g_cfg.pin_rst, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(g_cfg.pin_rst, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
}

// --- Display operations ---

// The 13.3" panel has two gate driver ICs: CS selects the left half,
// CS1 selects the right half. LOW = selected.
// Commands 0xF0 through TRES go to both controllers (CS1 LOW).
// Commands 0x74 and PWR through 0xB1 go to left controller only (CS1 HIGH).
static void send_init_sequence(void)
{
    wait_busy("init");

    // 0x74 — CS1 stays HIGH (left controller only)
    cmd_data(0x74, r74DataBuf, sizeof(r74DataBuf));

    // 0xF0 through TRES — both controllers
    cmd_data_both(0xF0, rf0DataBuf, sizeof(rf0DataBuf));
    cmd_data_both(0x00, PSR_V, sizeof(PSR_V));
    cmd_data_both(0x50, CDI_V, sizeof(CDI_V));
    cmd_data_both(0x60, r60DataBuf, sizeof(r60DataBuf));
    cmd_data_both(0x86, r86DataBuf, sizeof(r86DataBuf));
    cmd_data_both(0xE3, PWS_V, sizeof(PWS_V));
    cmd_data_both(0x61, TRES_V, sizeof(TRES_V));

    // PWR through 0xB1 — left controller only (CS1 stays HIGH)
    cmd_data(0x01, PWR_V, sizeof(PWR_V));
    vTaskDelay(pdMS_TO_TICKS(10));
    cmd_data(0xB6, rb6DataBuf, sizeof(rb6DataBuf));
    vTaskDelay(pdMS_TO_TICKS(10));
    cmd_data(0x06, BTST_P_V, sizeof(BTST_P_V));
    vTaskDelay(pdMS_TO_TICKS(10));
    cmd_data(0xB7, rb7DataBuf, sizeof(rb7DataBuf));
    vTaskDelay(pdMS_TO_TICKS(10));
    cmd_data(0x05, BTST_N_V, sizeof(BTST_N_V));
    vTaskDelay(pdMS_TO_TICKS(10));
    cmd_data(0xB0, rb0DataBuf, sizeof(rb0DataBuf));
    vTaskDelay(pdMS_TO_TICKS(10));
    cmd_data(0xB1, rb1DataBuf, sizeof(rb1DataBuf));
    vTaskDelay(pdMS_TO_TICKS(10));
}

// Convert 4-bit color index to hardware color value
static uint8_t color_get(uint8_t c)
{
    switch (c) {
    case 0:
        return 0x00;  // Black
    case 1:
        return 0x01;  // White
    case 2:
        return 0x02;  // Yellow
    case 3:
        return 0x03;  // Red
    case 5:
        return 0x05;  // Blue
    case 6:
        return 0x06;  // Green
    default:
        return 0x01;  // Default to White
    }
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

    ESP_LOGI(TAG, "Initializing ED2208-NCA (Spectra 6, 13.3\") E-Paper Driver");

    spi_add_device();
    gpio_init();

#ifdef CONFIG_PM_ENABLE
    esp_err_t ret = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "epd_update", &pm_lock);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create PM lock: %s", esp_err_to_name(ret));
    }
#endif

    hw_reset();
    send_init_sequence();
}

void epaper_clear(uint8_t *image, uint8_t color)
{
    uint8_t c = color_get(color);
    uint8_t packed = (c << 4) | c;
    memset(image, packed, EPD_BUF_SIZE);

    ESP_LOGI(TAG, "Clearing display with color 0x%02x", color);
    epaper_display(image);
    ESP_LOGI(TAG, "Clear complete");
}

void epaper_display(uint8_t *image)
{
#ifdef CONFIG_PM_ENABLE
    if (pm_lock) {
        esp_pm_lock_acquire(pm_lock);
    }
#endif

    ESP_LOGI(TAG, "Starting display update: %d bytes", EPD_BUF_SIZE);

    // CCSET — both controllers
    cmd_data_both(0xE0, CCSET_V_CUR, sizeof(CCSET_V_CUR));

    wait_busy("ccset");

    // Split transfer: left half via CS, right half via CS1
    uint16_t block_w_bytes = EPD_WIDTH / 2 / 2;  // 300 bytes (packed)
    uint16_t row_stride_bytes = EPD_WIDTH / 2;   // 600 bytes

    uint8_t *line_buf = heap_caps_malloc(block_w_bytes, MALLOC_CAP_DMA);
    if (!line_buf) {
        ESP_LOGE(TAG, "Malloc failed");
        goto out;
    }

    // --- Phase 1: CS (Left half) ---
    gpio_set_level(g_cfg.pin_cs, 0);

    gpio_set_level(g_cfg.pin_dc, 0);  // CMD
    uint8_t dtm_cmd = 0x10;
    spi_transaction_t t_dtm = {.length = 8, .tx_buffer = &dtm_cmd};
    spi_device_transmit(spi, &t_dtm);

    gpio_set_level(g_cfg.pin_dc, 1);  // DATA

    for (uint16_t row = 0; row < EPD_HEIGHT; row++) {
        for (uint16_t col = 0; col < block_w_bytes; col++) {
            uint8_t b = image[row * row_stride_bytes + col];
            uint8_t p1 = (b >> 4) & 0x0F;
            uint8_t p2 = b & 0x0F;
            line_buf[col] = (color_get(p1) << 4) | color_get(p2);
        }
        spi_transaction_t t_line = {.length = block_w_bytes * 8, .tx_buffer = line_buf};
        spi_device_transmit(spi, &t_line);
    }
    gpio_set_level(g_cfg.pin_cs, 1);

    // --- Phase 2: CS1 (Right half) ---
    gpio_set_level(g_cfg.pin_cs1, 0);

    gpio_set_level(g_cfg.pin_dc, 0);  // CMD
    spi_device_transmit(spi, &t_dtm);

    gpio_set_level(g_cfg.pin_dc, 1);  // DATA

    for (uint16_t row = 0; row < EPD_HEIGHT; row++) {
        for (uint16_t col = 0; col < block_w_bytes; col++) {
            uint8_t b = image[row * row_stride_bytes + col + block_w_bytes];
            uint8_t p1 = (b >> 4) & 0x0F;
            uint8_t p2 = b & 0x0F;
            line_buf[col] = (color_get(p1) << 4) | color_get(p2);
        }
        spi_transaction_t t_line = {.length = block_w_bytes * 8, .tx_buffer = line_buf};
        spi_device_transmit(spi, &t_line);
    }
    gpio_set_level(g_cfg.pin_cs1, 1);

    free(line_buf);

    // PON — both controllers
    cmd_data_both(0x04, NULL, 0);
    wait_busy("power_on");

    // DRF — both controllers
    cmd_data_both(0x12, DRF_V, sizeof(DRF_V));
    wait_busy("refresh");

    // POF — both controllers
    cmd_data_both(0x02, POF_V, sizeof(POF_V));
    wait_busy("power_off");

    ESP_LOGI(TAG, "Display update complete");

out:
#ifdef CONFIG_PM_ENABLE
    if (pm_lock) {
        esp_pm_lock_release(pm_lock);
    }
#endif
    return;
}

void epaper_enter_deepsleep(void)
{
    ESP_LOGI(TAG, "Entering deep sleep");

#ifdef CONFIG_PM_ENABLE
    if (pm_lock) {
        esp_pm_lock_acquire(pm_lock);
    }
#endif

    cmd_data(0x07, (uint8_t[]){0xA5}, 1);  // DEEP_SLEEP
    vTaskDelay(pdMS_TO_TICKS(1));
    wait_busy("deepsleep");

    if (g_cfg.pin_enable >= 0) {
        // Drive panel-facing GPIOs LOW before cutting VDD so they don't
        // back-feed through the panel's ESD diodes once its rail drops to
        // 0V. SPI peripheral pads (MOSI/SCLK) become Hi-Z in deep sleep
        // and don't need handling.
        gpio_set_level(g_cfg.pin_cs, 0);
        gpio_set_level(g_cfg.pin_dc, 0);
        gpio_set_level(g_cfg.pin_rst, 0);
        if (g_cfg.pin_cs1 >= 0) {
            gpio_set_level(g_cfg.pin_cs1, 0);
        }

        vTaskDelay(pdMS_TO_TICKS(100));       // Ensure display enters sleep before cutting power
        gpio_set_level(g_cfg.pin_enable, 0);  // Cut power
        // Latch the pads low so the rail stays cut and panel-facing
        // signals stay grounded once the digital IO domain powers down
        // during deep sleep, and arm deep-sleep hold so the latch
        // survives into deep sleep. All calls are idempotent.
        gpio_hold_en(g_cfg.pin_enable);
        gpio_hold_en(g_cfg.pin_cs);
        gpio_hold_en(g_cfg.pin_dc);
        gpio_hold_en(g_cfg.pin_rst);
        if (g_cfg.pin_cs1 >= 0) {
            gpio_hold_en(g_cfg.pin_cs1);
        }
        gpio_deep_sleep_hold_en();
    }

#ifdef CONFIG_PM_ENABLE
    if (pm_lock) {
        esp_pm_lock_release(pm_lock);
    }
#endif
}
