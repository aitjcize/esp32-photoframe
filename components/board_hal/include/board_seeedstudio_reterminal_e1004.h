#ifndef BOARD_SEEEDSTUDIO_RETERMINAL_E1004_H
#define BOARD_SEEEDSTUDIO_RETERMINAL_E1004_H

#include "driver/gpio.h"

// Board Info
#define BOARD_HAL_NAME "seeedstudio_reterminal_e1004"
#define BOARD_HAL_TYPE BOARD_TYPE_SEEEDSTUDIO_RETERMINAL_E1004

// Button Definitions (active-low), mapped to the E1004's physical buttons:
//   Refresh (GPIO5) -> Wake, Next/right (GPIO3) -> Rotate, Prev/left (GPIO4) -> Clear
#define BOARD_HAL_WAKEUP_KEY GPIO_NUM_5  // Refresh button (Wake/Select)
#define BOARD_HAL_WAKEUP_KEY_NAME "Refresh Button"
#define BOARD_HAL_ROTATE_KEY GPIO_NUM_3  // Next / right button (Rotate)
#define BOARD_HAL_CLEAR_KEY GPIO_NUM_4   // Prev / left button (Clear)

// SPI Pins (shared by the 13.3" panel and the microSD card)
#define BOARD_HAL_SPI_SCLK_PIN GPIO_NUM_7
#define BOARD_HAL_SPI_MOSI_PIN GPIO_NUM_9
#define BOARD_HAL_SPI_MISO_PIN GPIO_NUM_8

// E-Paper Display Pins (E-Ink ED2208-NCA, 13.3" Spectra 6, dual-CS panel)
#define BOARD_HAL_EPD_DC_PIN GPIO_NUM_11
#define BOARD_HAL_EPD_CS_PIN GPIO_NUM_10  // SCREEN_CS# / SPLCS_M (master half)
#define BOARD_HAL_EPD_CS1_PIN GPIO_NUM_2  // SPI_CS_S / SPLCS_S (slave half)
#define BOARD_HAL_EPD_RST_PIN GPIO_NUM_38
#define BOARD_HAL_EPD_BUSY_PIN GPIO_NUM_13
#define BOARD_HAL_EPD_ENABLE_PIN GPIO_NUM_12  // SCREEN_EN# (active-high power switch)

// SD Card Pins
#define BOARD_HAL_SD_PWR_PIN GPIO_NUM_16
#define BOARD_HAL_SD_CS_PIN GPIO_NUM_14

// I2C Pins (I2C0: RTC + T/RH sensor)
#define BOARD_HAL_I2C_SDA_PIN GPIO_NUM_19
#define BOARD_HAL_I2C_SCL_PIN GPIO_NUM_20

// Power Management
#define BOARD_HAL_BAT_ADC_PIN ADC_CHANNEL_0  // GPIO 1
#define BOARD_HAL_BAT_EN_PIN GPIO_NUM_21

// Onboard LED (active-high, NPN low-side drive)
#define BOARD_HAL_LED_PIN GPIO_NUM_48
#define BOARD_HAL_LED_INVERTED false

// Display Configuration
#define BOARD_HAL_DISPLAY_ROTATION_DEG 0

// The 13.3" panel and the microSD card share the SPI bus, and the SD's MISO
// (GPIO8) carries the extra load of the panel's SPLD1 FPC stub. Automatic
// light sleep powers down the CPU and isolates GPIOs, which disturbs the bus
// mid-transaction and makes SD block reads time out (ESP_ERR_TIMEOUT / 0x107)
// once WiFi/PM activity starts. Disable auto light sleep on this board (CPU
// frequency scaling is kept). The SD is mounted for the whole awake session,
// so light sleep would never engage here anyway.
#define BOARD_HAL_DISABLE_AUTO_LIGHT_SLEEP 1

#endif  // BOARD_SEEEDSTUDIO_RETERMINAL_E1004_H
