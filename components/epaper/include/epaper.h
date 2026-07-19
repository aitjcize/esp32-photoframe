#ifndef EPAPER_H
#define EPAPER_H

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
    int pin_rst;
    int pin_busy;
    int pin_cs1;     // For 13.3" Dual CS
    int pin_enable;  // Optional power enable
    uint8_t btst2[4];  // Booster Soft Start Phase 2 register values
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
