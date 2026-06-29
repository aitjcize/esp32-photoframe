// IT8951 e-paper T-CON driver (grayscale / GC16).
//
// Implements the epaper.h interface for IT8951-based panels such as the Seeed
// reTerminal E1003 (10.3" ED103TC2, 1872x1404, 16-level grayscale). The IT8951
// drives the panel waveforms + TPS65185 bias internally; the ESP32 talks to it
// over SPI with a manual CS plus RST and HRDY (host-ready / "busy") lines, and
// streams 4-bit-per-pixel gray data into the controller's frame buffer.
//
// Protocol reference: ITE IT8951 / Waveshare IT8951 HAT sample code.
//
// HARDWARE-VALIDATION NOTES (untested without the board):
//   - 4bpp pixel/endian order: the server packs 2 px/byte (high nibble = first
//     pixel). If the panel shows pixel pairs swapped, flip IT8951_LD_ENDIAN
//     (or swap nibbles in the server's createEPDGZ). This is the #1 IT8951
//     bring-up gotcha.
//   - VCOM: we read (and log) the value stored in the IT8951 waveform flash
//     rather than override it. If contrast is off, set it explicitly to the
//     value printed on the panel FPC.

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "epaper.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "it8951";

// ---- IT8951 commands ----
#define IT8951_TCON_SYS_RUN 0x0001
#define IT8951_TCON_STANDBY 0x0002
#define IT8951_TCON_SLEEP 0x0003
#define IT8951_TCON_REG_RD 0x0010
#define IT8951_TCON_REG_WR 0x0011
#define IT8951_TCON_LD_IMG_AREA 0x0021
#define IT8951_TCON_LD_IMG_END 0x0022
#define IT8951_CMD_DPY_AREA 0x0034
#define IT8951_CMD_GET_DEV_INFO 0x0302
#define IT8951_CMD_VCOM 0x0039
#define IT8951_CMD_TEMP 0x0040

// ---- Registers ----
#define IT8951_REG_LISAR 0x0208    // Load Image Start Addr (32-bit: lo@+0, hi@+2)
#define IT8951_REG_LUTAFSR 0x1224  // LUT/display busy status (0 = idle)
#define IT8951_REG_I80CPCR 0x0004  // Host packed-write control

// ---- SPI preambles (16-bit, MSB first) ----
#define IT8951_PRE_CMD 0x6000
#define IT8951_PRE_WR_DATA 0x0000
#define IT8951_PRE_RD_DATA 0x1000

// ---- Image load: 4bpp, little-endian, no rotation ----
#define IT8951_BPP_4 2  // pixel-format field: 0=2bpp,1=3bpp,2=4bpp,3=8bpp
#define IT8951_LD_ENDIAN 0
#define IT8951_LD_ROTATE 0

// ---- Display update modes (ED103TC2 / 10.3") ----
#define IT8951_MODE_INIT 0
#define IT8951_MODE_GC16 2
#define IT8951_MODE_A2 6

// Fallback VCOM magnitude (-2.0 V) applied only if the panel's stored VCOM is
// missing/invalid. A blank panel with VCOM ~= 0 is the classic symptom. The
// exact value is printed on the panel's FPC cable; tune if contrast is off.
#define IT8951_DEFAULT_VCOM_MV 2000

// The IT8951 picks its display waveform by panel temperature; if it's never
// forced, GC16 can refresh blank. Seeed's driver forces it before every update.
// A fixed room-temperature value is sufficient (the waveform tolerance is wide).
#define IT8951_DEFAULT_TEMP_C 20

// Seeed's own driver clocks IT8951 *reads* at 4 MHz (SPI_READ_FREQUENCY); reads
// above that return stale MISO (the GetSystemInfo high word came back garbled).
// The GC16 refresh dominates update time (~30 s), so we run the whole device at
// the read-safe 4 MHz rather than juggling a separate write clock.
#define IT8951_SPI_CLOCK_HZ (4 * 1000 * 1000)
#define IT8951_SPI_CHUNK 4000  // bytes per data burst (must be <= bus max_transfer_sz)
#define IT8951_BUSY_TIMEOUT_US (5 * 1000 * 1000)

typedef struct {
    uint16_t panel_w;
    uint16_t panel_h;
    uint16_t img_addr_l;
    uint16_t img_addr_h;
    uint16_t fw_version[8];
    uint16_t lut_version[8];
} it8951_dev_info_t;

static spi_device_handle_t s_spi = NULL;
// Internal-RAM, DMA-capable bounce buffer: the framebuffer lives in PSRAM, which
// SPI DMA can't transmit from directly, so image chunks are copied through this.
static uint8_t *s_dma_buf = NULL;
static int s_pin_cs = -1;
static int s_pin_rst = -1;
static int s_pin_busy = -1;    // HRDY: high = ready, low = busy
static int s_pin_enable = -1;  // EPD bias (TPS65185) enable
static uint32_t s_img_addr = 0;
static int8_t s_temp_c = IT8951_DEFAULT_TEMP_C;  // panel temperature for waveform select
// Default to the ED103TC2 geometry until GetSystemInfo reports the real values.
static it8951_dev_info_t s_dev = {.panel_w = 1872, .panel_h = 1404};

// ---- Low-level SPI helpers (manual CS, MSB-first 16-bit words) ----

static inline void cs_low(void)
{
    gpio_set_level(s_pin_cs, 0);
}
static inline void cs_high(void)
{
    gpio_set_level(s_pin_cs, 1);
}

// Wait for the controller to assert HRDY (ready). Returns false on timeout.
static bool wait_ready(void)
{
    int64_t deadline = esp_timer_get_time() + IT8951_BUSY_TIMEOUT_US;
    while (gpio_get_level(s_pin_busy) == 0) {
        if (esp_timer_get_time() > deadline) {
            ESP_LOGE(TAG, "HRDY timeout");
            return false;
        }
        vTaskDelay(1);
    }
    return true;
}

static void spi_tx(const uint8_t *tx, size_t len)
{
    spi_transaction_t t = {.length = len * 8, .tx_buffer = tx};
    esp_err_t ret = spi_device_polling_transmit(s_spi, &t);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI tx %d bytes failed: %s", (int) len, esp_err_to_name(ret));
    }
}

static void spi_txrx(const uint8_t *tx, uint8_t *rx, size_t len)
{
    spi_transaction_t t = {.length = len * 8, .tx_buffer = tx, .rx_buffer = rx};
    spi_device_polling_transmit(s_spi, &t);
}

static void spi_write16(uint16_t v)
{
    uint8_t b[2] = {(uint8_t) (v >> 8), (uint8_t) (v & 0xFF)};
    spi_tx(b, 2);
}

static uint16_t spi_read16(void)
{
    uint8_t tx[2] = {0, 0};
    uint8_t rx[2] = {0, 0};
    spi_txrx(tx, rx, 2);
    return ((uint16_t) rx[0] << 8) | rx[1];
}

static void it8951_write_cmd(uint16_t cmd)
{
    wait_ready();
    cs_low();
    spi_write16(IT8951_PRE_CMD);
    wait_ready();
    spi_write16(cmd);
    cs_high();
}

static void it8951_write_data(uint16_t data)
{
    wait_ready();
    cs_low();
    spi_write16(IT8951_PRE_WR_DATA);
    wait_ready();
    spi_write16(data);
    cs_high();
}

static uint16_t it8951_read_data(void)
{
    wait_ready();
    cs_low();
    spi_write16(IT8951_PRE_RD_DATA);
    wait_ready();
    (void) spi_read16();  // dummy word (read latency)
    wait_ready();
    uint16_t v = spi_read16();
    cs_high();
    return v;
}

static void it8951_read_data_buf(uint16_t *buf, size_t words)
{
    wait_ready();
    cs_low();
    spi_write16(IT8951_PRE_RD_DATA);
    wait_ready();
    (void) spi_read16();  // dummy word (read latency)
    wait_ready();
    // Read all words in ONE continuous transaction. Reading word-by-word leaves
    // gaps between SPI transactions where the IT8951 can re-present stale MISO
    // data (observed: the high address word read back as a copy of the low word).
    uint8_t rx[64] = {0};
    const size_t nbytes = words * 2;
    if (nbytes <= sizeof(rx)) {
        spi_transaction_t t = {.length = nbytes * 8, .rxlength = nbytes * 8, .rx_buffer = rx};
        esp_err_t ret = spi_device_polling_transmit(s_spi, &t);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "SPI read %d bytes failed: %s", (int) nbytes, esp_err_to_name(ret));
        }
        for (size_t i = 0; i < words; i++) {
            buf[i] = ((uint16_t) rx[i * 2] << 8) | rx[i * 2 + 1];  // MSB-first
        }
    } else {
        for (size_t i = 0; i < words; i++) {
            buf[i] = spi_read16();
        }
    }
    cs_high();
}

static void it8951_write_reg(uint16_t reg, uint16_t val)
{
    it8951_write_cmd(IT8951_TCON_REG_WR);
    it8951_write_data(reg);
    it8951_write_data(val);
}

static uint16_t it8951_read_reg(uint16_t reg)
{
    it8951_write_cmd(IT8951_TCON_REG_RD);
    it8951_write_data(reg);
    return it8951_read_data();
}

// ---- IT8951 operations ----

static void it8951_reset(void)
{
    gpio_set_level(s_pin_rst, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(s_pin_rst, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
}

static bool it8951_get_dev_info(void)
{
    for (int attempt = 0; attempt < 8; attempt++) {
        it8951_write_cmd(IT8951_CMD_GET_DEV_INFO);
        it8951_read_data_buf((uint16_t *) &s_dev, sizeof(s_dev) / 2);
        uint32_t addr = ((uint32_t) s_dev.img_addr_h << 16) | s_dev.img_addr_l;
        // Validate against a flaky SPI read: sane panel size + an image-buffer
        // address inside the IT8951's external RAM window.
        if (s_dev.panel_w >= 100 && s_dev.panel_w <= 4096 && s_dev.panel_h >= 100 &&
            s_dev.panel_h <= 4096 && addr >= 0x100000 && addr < 0x04000000) {
            s_img_addr = addr;
            return true;
        }
        ESP_LOGW(TAG, "dev info invalid (panel %ux%u, addr 0x%08lx), retry", s_dev.panel_w,
                 s_dev.panel_h, (unsigned long) addr);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    ESP_LOGE(TAG, "GetSystemInfo failed validation after retries");
    return false;
}

static int16_t it8951_get_vcom(void)
{
    it8951_write_cmd(IT8951_CMD_VCOM);
    it8951_write_data(0);  // 0 = read
    return (int16_t) it8951_read_data();
}

static void it8951_set_vcom(uint16_t mv)
{
    it8951_write_cmd(IT8951_CMD_VCOM);
    it8951_write_data(1);  // 1 = set
    it8951_write_data(mv);
}

// Force the panel temperature (selects the display waveform).
static void it8951_set_temp(int16_t celsius)
{
    it8951_write_cmd(IT8951_CMD_TEMP);
    it8951_write_data(1);  // 1 = force-write temperature
    it8951_write_data((uint16_t) celsius);
}

static void it8951_set_target_memory(uint32_t addr)
{
    it8951_write_reg(IT8951_REG_LISAR + 2, (uint16_t) (addr >> 16));
    it8951_write_reg(IT8951_REG_LISAR, (uint16_t) (addr & 0xFFFF));
}

// Poll the display/LUT engine until idle.
static void it8951_wait_display_ready(void)
{
    int64_t deadline = esp_timer_get_time() + IT8951_BUSY_TIMEOUT_US;
    while (it8951_read_reg(IT8951_REG_LUTAFSR) != 0) {
        if (esp_timer_get_time() > deadline) {
            ESP_LOGE(TAG, "display LUT timeout");
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void it8951_display_area(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t mode)
{
    it8951_wait_display_ready();
    it8951_write_cmd(IT8951_CMD_DPY_AREA);
    it8951_write_data(x);
    it8951_write_data(y);
    it8951_write_data(w);
    it8951_write_data(h);
    it8951_write_data(mode);
}

// ---- epaper.h interface ----

void epaper_init(const epaper_config_t *cfg)
{
    s_pin_cs = cfg->pin_cs;
    s_pin_rst = cfg->pin_rst;
    s_pin_busy = cfg->pin_busy;      // HRDY
    s_pin_enable = cfg->pin_enable;  // EPD bias enable (optional)

    // GPIOs: CS/RST/EN as outputs, HRDY as input.
    gpio_config_t out_cfg = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << s_pin_cs) | (1ULL << s_pin_rst),
    };
    if (s_pin_enable >= 0) {
        out_cfg.pin_bit_mask |= (1ULL << s_pin_enable);
    }
    gpio_config(&out_cfg);
    gpio_config_t in_cfg = {
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << s_pin_busy),
    };
    gpio_config(&in_cfg);

    cs_high();
    gpio_set_level(s_pin_rst, 1);
    if (s_pin_enable >= 0) {
        gpio_set_level(s_pin_enable, 1);  // enable EPD bias (TPS65185)
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // SPI device with manual CS (CS must stay low across preamble + payload).
    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = IT8951_SPI_CLOCK_HZ,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 1,
    };
    esp_err_t ret = spi_bus_add_device(cfg->spi_host, &dev_cfg, &s_spi);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(ret));
        return;
    }

    // DMA-capable internal-RAM bounce buffer for streaming the PSRAM framebuffer.
    s_dma_buf = heap_caps_malloc(IT8951_SPI_CHUNK, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_dma_buf) {
        ESP_LOGE(TAG, "failed to alloc %d-byte SPI DMA buffer", IT8951_SPI_CHUNK);
    }

    it8951_reset();
    if (!wait_ready()) {
        ESP_LOGE(TAG, "IT8951 not responding after reset");
        return;
    }

    it8951_write_cmd(IT8951_TCON_SYS_RUN);
    if (!it8951_get_dev_info()) {
        ESP_LOGE(TAG, "aborting init: no valid device info");
        return;
    }

    int16_t vcom = it8951_get_vcom();
    if (vcom <= 0 || vcom > 5000) {
        ESP_LOGW(TAG, "VCOM invalid; applying -%d mV default", IT8951_DEFAULT_VCOM_MV);
        it8951_set_vcom(IT8951_DEFAULT_VCOM_MV);
    }

    // Enable host packed-write so streamed image data isn't byte-padded.
    it8951_write_reg(IT8951_REG_I80CPCR, 0x0001);

    // Force the panel temperature so the IT8951 selects a usable waveform;
    // without it GC16 can refresh blank. s_temp_c starts at a room-temperature
    // default and is refined by epaper_set_temperature() (live SHT40 reading).
    it8951_set_temp(s_temp_c);
}

void epaper_set_temperature(int8_t celsius)
{
    s_temp_c = celsius;
    if (s_spi) {
        it8951_set_temp(celsius);
    }
}

void epaper_display(uint8_t *image)
{
    if (!s_spi || !image || !s_dma_buf) {
        return;
    }
    const uint16_t w = s_dev.panel_w;
    const uint16_t h = s_dev.panel_h;
    const size_t row_bytes = (size_t) w / 2;  // 4bpp = 2 px/byte

    it8951_set_target_memory(s_img_addr);

    // Load full-frame image area (4bpp).
    it8951_write_cmd(IT8951_TCON_LD_IMG_AREA);
    it8951_write_data((IT8951_LD_ENDIAN << 8) | (IT8951_BPP_4 << 4) | IT8951_LD_ROTATE);
    it8951_write_data(0);  // x
    it8951_write_data(0);  // y
    it8951_write_data(w);
    it8951_write_data(h);

    // Stream the image in ONE CS-low session (single 0x0000 write preamble, all
    // data back-to-back; toggling CS mid-load resets the IT8951 write pointer).
    // The ED103TC2 scans each row right-to-left, so mirror it -- but at 16-bit
    // *word* granularity (the unit the IT8951 reconstructs from the SPI byte
    // stream): emit the row's words in reverse order while keeping each word's
    // two bytes intact, exactly as Seeed's driver does. Reversing at byte
    // granularity would swap the two bytes inside each word and scramble pixels
    // locally. The bounce buffer also keeps SPI DMA off the PSRAM frame buffer
    // (which it can't transmit from directly).
    const size_t row_words = row_bytes / 2;
    wait_ready();
    cs_low();
    spi_write16(IT8951_PRE_WR_DATA);
    for (uint16_t y = 0; y < h; y++) {
        const uint8_t *src = image + (size_t) y * row_bytes;
        for (size_t wi = 0; wi < row_words; wi++) {
            const uint8_t *sw = src + (row_words - 1 - wi) * 2;
            s_dma_buf[wi * 2] = sw[0];
            s_dma_buf[wi * 2 + 1] = sw[1];
        }
        spi_tx(s_dma_buf, row_bytes);
    }
    cs_high();

    it8951_write_cmd(IT8951_TCON_LD_IMG_END);

    // Anti-ghosting: a full INIT (white) clear resets every pixel before the
    // image, so high-contrast content (e.g. a QR code) doesn't shadow through.
    // GC16 alone leaves visible ghosting on this panel, so we accept the extra
    // clear flash. The image is already loaded, so the prior frame stays up
    // during the load and only the brief clear precedes the new image.
    it8951_display_area(0, 0, w, h, IT8951_MODE_INIT);
    it8951_wait_display_ready();

    it8951_display_area(0, 0, w, h, IT8951_MODE_GC16);
    it8951_wait_display_ready();
}

void epaper_clear(uint8_t *image, uint8_t color)
{
    (void) image;
    (void) color;
    // INIT-mode refresh clears the panel to white and removes ghosting,
    // independent of frame-buffer contents.
    if (!s_spi) {
        return;
    }
    it8951_display_area(0, 0, s_dev.panel_w, s_dev.panel_h, IT8951_MODE_INIT);
    it8951_wait_display_ready();
}

void epaper_enter_deepsleep(void)
{
    if (s_spi) {
        it8951_write_cmd(IT8951_TCON_SLEEP);
    }
    // Drop EPD bias to save power; the board cuts IT8951 core power separately.
    if (s_pin_enable >= 0) {
        gpio_set_level(s_pin_enable, 0);
    }
}

uint16_t epaper_get_width(void)
{
    return s_dev.panel_w;
}

uint16_t epaper_get_height(void)
{
    return s_dev.panel_h;
}
