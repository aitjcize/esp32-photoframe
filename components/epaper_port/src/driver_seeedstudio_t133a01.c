#include <assert.h>
#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

#include "epaper_port.h"

static const char *TAG = "epaper_seeedstudio_t133a01";

// Pin definition for Seeed XIAO EE02 (verified from Seeed_GFX)
// Local config storage
static epaper_config_t g_ep_config = {0};

#define EPD_DC_PIN g_ep_config.pin_dc
#define EPD_CS_PIN g_ep_config.pin_cs
#define EPD_CS1_PIN g_ep_config.pin_cs1
#define EPD_SCK_PIN g_ep_config.pin_sck
#define EPD_MOSI_PIN g_ep_config.pin_mosi
#define EPD_RST_PIN g_ep_config.pin_rst
#define EPD_BUSY_PIN g_ep_config.pin_busy
#define EPD_ENABLE_PIN g_ep_config.pin_enable

#define EPD_HOST SPI2_HOST
#define EPD_WIDTH 1200
#define EPD_HEIGHT 1600

// Use 10MHz to match Seeed_GFX (SPI_FREQUENCY)
#define SPI_SPEED_HZ (10 * 1000 * 1000)

static spi_device_handle_t spi;

// --- Command Definitions (from T133A01_Defines.h) ---
#define R00_PSR 0x00
#define R01_PWR 0x01
#define R02_POF 0x02
#define R04_PON 0x04
#define R05_BTST_N 0x05
#define R06_BTST_P 0x06
#define R10_DTM 0x10
#define R12_DRF 0x12
#define R50_CDI 0x50
#define R61_TRES 0x61
#define RE0_CCSET 0xE0
#define RE3_PWS 0xE3
#define RE5_TSSET 0xE5

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

// Resolution
uint16_t epaper_get_width(void)
{
    return 1200;
}
uint16_t epaper_get_height(void)
{
    return 1600;
}

// --- SPI Helpers (Manual CS) ---

static void epd_spi_init(void)
{
    esp_err_t ret;
    spi_bus_config_t buscfg = {
        .miso_io_num = -1,
        .mosi_io_num = EPD_MOSI_PIN,
        .sclk_io_num = EPD_SCK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = EPD_WIDTH * EPD_HEIGHT / 2 + 100,  // Sufficient buffer
    };

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = SPI_SPEED_HZ,
        .mode = 0,
        .spics_io_num = -1,  // Manual CS control required for dual-CS logic
        .queue_size = 7,
    };

    ret = spi_bus_initialize(EPD_HOST, &buscfg, SPI_DMA_CH_AUTO);
    ESP_ERROR_CHECK(ret);

    ret = spi_bus_add_device(EPD_HOST, &devcfg, &spi);
    ESP_ERROR_CHECK(ret);

    // Initialize GPIOs
    gpio_set_direction(EPD_DC_PIN, GPIO_MODE_OUTPUT);
    gpio_set_direction(EPD_RST_PIN, GPIO_MODE_OUTPUT);
    gpio_set_direction(EPD_BUSY_PIN, GPIO_MODE_INPUT);

    // Manual CS Pins
    gpio_set_direction(EPD_CS_PIN, GPIO_MODE_OUTPUT);
    gpio_set_direction(EPD_CS1_PIN, GPIO_MODE_OUTPUT);  // CS1 (41)
    gpio_set_level(EPD_CS_PIN, 1);
    gpio_set_level(EPD_CS1_PIN, 1);

    // Enable Pin
    gpio_set_direction(EPD_ENABLE_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(EPD_ENABLE_PIN, 1);  // Enable HIGH
}

static void CS_L(void)
{
    gpio_set_level(EPD_CS_PIN, 0);
}
static void CS_H(void)
{
    gpio_set_level(EPD_CS_PIN, 1);
}

// Seeed_GFX: writecommanddata toggles CS (the standard one, likely CS_PIN)
static void writecommanddata(uint8_t cmd, const uint8_t *data, size_t len)
{
    CS_L();

    // Send Command
    gpio_set_level(EPD_DC_PIN, 0);
    spi_transaction_t t_cmd = {.length = 8, .tx_buffer = &cmd};
    spi_device_transmit(spi, &t_cmd);

    // Send Data
    if (len > 0) {
        gpio_set_level(EPD_DC_PIN, 1);
        spi_transaction_t t_data = {.length = len * 8, .tx_buffer = data};
        spi_device_transmit(spi, &t_data);
    }

    CS_H();
}

static void wait_busy(void)
{
    ESP_LOGI(TAG, "Waiting for BUSY...");
    // loops until HIGH. So LOW is BUSY.
    while (gpio_get_level(EPD_BUSY_PIN) == 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGI(TAG, "BUSY released");
}

static void epd_reset(void)
{
    gpio_set_level(EPD_RST_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(EPD_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
}

// Mimic EPD_INIT macro
static void epd_init_sequence(void)
{
    wait_busy();

    // Note: EPD_INIT macro uses TFT_CS1 for toggling around send command
    // "digitalWrite(TFT_CS1, LOW); ... writecommanddata(...) ... digitalWrite(TFT_CS1, HIGH);"
    // Since writecommanddata toggles CS, we have CS1 LOW + CS Toggling.

#define SEND_CMD_CS1(c, d, l)           \
    do {                                \
        gpio_set_level(EPD_CS1_PIN, 0); \
        writecommanddata(c, d, l);      \
        gpio_set_level(EPD_CS1_PIN, 1); \
        vTaskDelay(pdMS_TO_TICKS(10));  \
    } while (0)

    SEND_CMD_CS1(0x74, r74DataBuf, sizeof(r74DataBuf));
    SEND_CMD_CS1(0xF0, rf0DataBuf, sizeof(rf0DataBuf));
    SEND_CMD_CS1(R00_PSR, PSR_V, sizeof(PSR_V));
    SEND_CMD_CS1(R50_CDI, CDI_V, sizeof(CDI_V));
    SEND_CMD_CS1(0x60, r60DataBuf, sizeof(r60DataBuf));
    SEND_CMD_CS1(0x86, r86DataBuf, sizeof(r86DataBuf));
    SEND_CMD_CS1(RE3_PWS, PWS_V, sizeof(PWS_V));
    SEND_CMD_CS1(R61_TRES, TRES_V, sizeof(TRES_V));

    // Power On
    SEND_CMD_CS1(R01_PWR, PWR_V, sizeof(PWR_V));

    SEND_CMD_CS1(0xB6, rb6DataBuf, sizeof(rb6DataBuf));
    vTaskDelay(pdMS_TO_TICKS(10));
    SEND_CMD_CS1(R06_BTST_P, BTST_P_V, sizeof(BTST_P_V));
    vTaskDelay(pdMS_TO_TICKS(10));
    SEND_CMD_CS1(0xB7, rb7DataBuf, sizeof(rb7DataBuf));
    vTaskDelay(pdMS_TO_TICKS(10));
    SEND_CMD_CS1(R05_BTST_N, BTST_N_V, sizeof(BTST_N_V));
    vTaskDelay(pdMS_TO_TICKS(10));
    SEND_CMD_CS1(0xB0, rb0DataBuf, sizeof(rb0DataBuf));
    vTaskDelay(pdMS_TO_TICKS(10));
    SEND_CMD_CS1(0xB1, rb1DataBuf, sizeof(rb1DataBuf));
    vTaskDelay(pdMS_TO_TICKS(10));
}

void epaper_port_init(const epaper_config_t *cfg)
{
    assert(cfg != NULL);
    memcpy(&g_ep_config, cfg, sizeof(epaper_config_t));

    ESP_LOGI(TAG, "Initializing XIAO EE02 E-Paper Driver");
    epd_spi_init();

    // EPD_WAKEUP calls EPD_INIT
    epd_reset();
    epd_init_sequence();
}

// Convert 4-bit color to 8-bit byte for SPI (Seeed logic)
// See COLOR_GET macro: maps 4-bit palette to 3-bit controller value?
// COLOR_GET: 0xF->0, 0x0->1, 0x2->6, 0xB->2, 0xD->5, 0x6->3, else->1
static uint8_t color_get(uint8_t c)
{
    // Map image processor indices to hardware commands
    // 0: Black, 1: White, 2: Yellow, 3: Red, 5: Blue, 6: Green
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

void epaper_port_clear(uint8_t *buffer, uint8_t color)
{
    uint8_t c = color_get(color);
    // Packed: 4-bit per pixel, 2 pixels per byte.
    // Logic: (p1 << 4) | p2.
    uint8_t packed = (c << 4) | c;

    uint32_t size = epaper_get_width() * epaper_get_height() / 2;
    memset(buffer, packed, size);
}

void epaper_port_enter_deepsleep(void)
{
    // Not implemented for T133A01 yet
}

void epaper_port_display(uint8_t *buffer)
{
    // Implementation of EPD_PUSH_NEW_COLORS
    // Note: buffer is expected to be 4-bit per pixel (packed 2 pixels per byte)?
    // Or 8-bit? Seeed_GFX default for 4-bit sprites is 1 pixel per byte (uint8_t array)?
    // "uint8_t b = (colors[(bytes_per_block_row * 2) *row+col])" implies 1 byte per pixel input
    // array? User buffer is usually uint8_t. Assuming 1 byte per pixel for simplicity or packed. If
    // packed, need unpacking. For now, assuming standard 4-bit packed or 8-bit unpacked.
    // EPD_PUSH_NEW_COLORS takes uint8_t *colors.
    // logic: temp1 = (b >> 4) & 0x0F; temp2 = b & 0x0F; -> Implies input is PACKED (2 pixels/byte).
    // And output to spi.transfer is combined: (COLOR_GET(temp1)<<4) | COLOR_GET(temp2).

    ESP_LOGI(TAG, "Display Update");

    // 1. CCSET
    gpio_set_level(EPD_CS1_PIN, 0);
    writecommanddata(RE0_CCSET, CCSET_V_CUR, sizeof(CCSET_V_CUR));
    gpio_set_level(EPD_CS1_PIN, 1);

    wait_busy();
    vTaskDelay(pdMS_TO_TICKS(10));

    // Split transfer: 1600x1200.
    // Master (CS) handles half? Slave (CS1) handles half?
    // Seeed Code: "uint16_t bytes_per_blcok_row = (w) / 4;"
    // Width = 1600. /4 = 400.
    // Loop col 0 to 400.
    // Wait, if input is packed (2 pix/byte), then 1600 pixels is 800 bytes.
    // bytes_per_block_row = 400. This is HALF a row in bytes.
    // So it sends Left half to CS, Right half to CS1? (Or interleaved?)

    uint16_t w = EPD_WIDTH;                // 1200
    uint16_t h = EPD_HEIGHT;               // 1600
    uint16_t block_w = w / 2;              // 600 pixels
    uint16_t block_w_bytes = block_w / 2;  // 300 bytes (packed)
    uint16_t row_stride_bytes = w / 2;     // 600 bytes

    // --- Phase 1: CS (Left half) ---
    CS_L();  // CS LOW

    gpio_set_level(EPD_DC_PIN, 0);  // CMD
    spi_transaction_t t_dtm = {.length = 8, .tx_buffer = (uint8_t[]) {R10_DTM}};
    spi_device_transmit(spi, &t_dtm);

    gpio_set_level(EPD_DC_PIN, 1);  // DATA

    uint8_t *line_buf = heap_caps_malloc(block_w_bytes, MALLOC_CAP_DMA);
    if (!line_buf) {
        ESP_LOGE(TAG, "Malloc failed");
        return;
    }

    for (uint16_t row = 0; row < h; row++) {
        for (uint16_t col = 0; col < block_w_bytes; col++) {
            // Index into the left half of the row
            uint8_t b = buffer[row * row_stride_bytes + col];
            uint8_t p1 = (b >> 4) & 0x0F;
            uint8_t p2 = b & 0x0F;
            line_buf[col] = (color_get(p1) << 4) | color_get(p2);
        }
        spi_transaction_t t_line = {.length = block_w_bytes * 8, .tx_buffer = line_buf};
        spi_device_transmit(spi, &t_line);
    }
    CS_H();  // CS HIGH

    // --- Phase 2: CS1 (Right half) ---
    gpio_set_level(EPD_CS1_PIN, 0);

    gpio_set_level(EPD_DC_PIN, 0);  // CMD
    spi_device_transmit(spi, &t_dtm);

    gpio_set_level(EPD_DC_PIN, 1);  // DATA

    for (uint16_t row = 0; row < h; row++) {
        for (uint16_t col = 0; col < block_w_bytes; col++) {
            // Index into the right half of the row (+300 bytes)
            uint8_t b = buffer[row * row_stride_bytes + col + block_w_bytes];
            uint8_t p1 = (b >> 4) & 0x0F;
            uint8_t p2 = b & 0x0F;
            line_buf[col] = (color_get(p1) << 4) | color_get(p2);
        }
        spi_transaction_t t_line = {.length = block_w_bytes * 8, .tx_buffer = line_buf};
        spi_device_transmit(spi, &t_line);
    }
    gpio_set_level(EPD_CS1_PIN, 1);  // CS1 HIGH

    free(line_buf);

    // Update Command: Verified against Seeed_GFX T133A01_Defines.h EPD_UPDATE
    // Sequence uses CS1 (TFT_CS1) exclusively.

    gpio_set_level(EPD_CS1_PIN, 0);
    writecommanddata(R04_PON, NULL, 0);
    gpio_set_level(EPD_CS1_PIN, 1);
    wait_busy();
    vTaskDelay(pdMS_TO_TICKS(30));  // 30ms delay

    gpio_set_level(EPD_CS1_PIN, 0);
    writecommanddata(R12_DRF, DRF_V, sizeof(DRF_V));
    gpio_set_level(EPD_CS1_PIN, 1);
    wait_busy();
    vTaskDelay(pdMS_TO_TICKS(30));  // 30ms delay

    gpio_set_level(EPD_CS1_PIN, 0);
    writecommanddata(R02_POF, POF_V, sizeof(POF_V));
    gpio_set_level(EPD_CS1_PIN, 1);
    wait_busy();
    vTaskDelay(pdMS_TO_TICKS(30));  // 30ms delay
}
