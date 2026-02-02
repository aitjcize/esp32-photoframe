#ifndef EPAPER_PORT_H
#define EPAPER_PORT_H

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
    int pin_cs;
    int pin_dc;
    int pin_rst;
    int pin_busy;
    int pin_sck;
    int pin_mosi;
    int pin_cs1;     // For 13.3" Dual CS
    int pin_enable;  // Optional power enable
} epaper_config_t;

void epaper_port_init(const epaper_config_t *cfg);
void epaper_port_clear(uint8_t *Image, uint8_t color);
void epaper_port_display(uint8_t *Image);
void epaper_port_enter_deepsleep(void);

#ifdef __cplusplus
}
#endif

#endif  // !EPAPER_PORT_H
