#ifndef EPAPER_H
#define EPAPER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Resolution APIs
uint16_t epaper_get_width(void);
uint16_t epaper_get_height(void);

/**********************************
Color Index
**********************************/
#define EPD_7IN3E_BLACK 0x0   /// 000
#define EPD_7IN3E_WHITE 0x1   /// 001
#define EPD_7IN3E_YELLOW 0x2  /// 010
#define EPD_7IN3E_RED 0x3     /// 011
#define EPD_7IN3E_BLUE 0x5    /// 101
#define EPD_7IN3E_GREEN 0x6   /// 110

#ifdef __cplusplus
extern "C" {
#endif

// Configuration Struct
typedef struct {
    int spi_host;
    int pin_cs;
    int pin_dc;
    int pin_rst;  // -1 if the controller has no reset line
    int pin_busy;
    int pin_cs1;     // For 13.3" Dual CS
    int pin_enable;  // Optional power enable

    // --- IT8951-only panel options; ignored by the other drivers ---
    // Panel geometry to fall back on if GetSystemInfo can't be read (0 = use the
    // driver's built-in default).
    int panel_w;
    int panel_h;
    // VCOM magnitude in mV to program explicitly, e.g. 2300 for -2.30 V (0 =
    // keep whatever is stored in the controller's waveform flash).
    int vcom_mv;
    // Load image data big-endian rather than little-endian. Which one is right
    // is a property of the panel wiring, and getting it wrong swaps pixel pairs.
    bool big_endian;
    // Emit each row's 16-bit words in reverse order, for panels that scan
    // right-to-left (the Seeed ED103TC2 does; the M5Paper ED047TC2 does not).
    bool mirror_x;

    // --- ED2208-GCA-only option ---
    // Program Good Display's GDEP073E01 register set (as used by GxEPD2 and the
    // paperlesspaper firmware) instead of the Waveshare/Seeed one.
    bool gca_gdep073e01_init;
} epaper_config_t;

/**
 * @brief Initialize the E-Paper display
 * @param cfg Configuration structure
 */
void epaper_init(const epaper_config_t *cfg);

/**
 * @brief Send image buffer to display
 * @param image Pointer to image buffer
 */
void epaper_display(uint8_t *image);

/**
 * @brief Clear display with specific color
 * @param image Buffer to use for clearing (size must match display)
 * @param color Color to clear with
 */
void epaper_clear(uint8_t *image, uint8_t color);

/**
 * @brief Set the panel temperature in degrees Celsius.
 *
 * Controllers that select their display waveform by temperature (e.g. IT8951)
 * use this to pick the right waveform; feeding a live reading improves grayscale
 * quality and reduces ghosting. Only implemented by drivers that need it.
 *
 * @param celsius Panel temperature in degrees Celsius
 */
void epaper_set_temperature(int8_t celsius);

/**
 * @brief Begin a streamed frame update.
 *
 * For boards without PSRAM, which cannot hold a frame buffer: the frame goes
 * straight into the controller's own frame memory, one chunk at a time, via
 * epaper_stream_write(), and epaper_stream_end() refreshes the panel. Only
 * drivers used on such boards implement this (currently ED2208-GCA).
 *
 * @param flip_180 Scan the frame rotated by 180 degrees, so a frame streamed
 *                 top row first can be shown upside down
 */
void epaper_stream_begin(bool flip_180);

/**
 * @brief Send the next chunk of a streamed frame: packed pixels, two per byte,
 * rows top to bottom.
 */
void epaper_stream_write(const uint8_t *data, size_t len);

/**
 * @brief Finish a streamed frame update.
 *
 * @param refresh Refresh the panel with the streamed frame; false abandons it
 *                and leaves the panel showing its previous picture
 */
void epaper_stream_end(bool refresh);

/**
 * @brief Enter deep sleep mode
 */
void epaper_enter_deepsleep(void);

/**
 * @brief Get display width
 * @return width in pixels
 */
uint16_t epaper_get_width(void);

/**
 * @brief Get display height
 * @return height in pixels
 */
uint16_t epaper_get_height(void);

#ifdef __cplusplus
}
#endif

#endif  // !EPAPER_H
