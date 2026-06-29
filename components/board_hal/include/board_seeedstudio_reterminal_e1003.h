#ifndef BOARD_SEEEDSTUDIO_RETERMINAL_E1003_H
#define BOARD_SEEEDSTUDIO_RETERMINAL_E1003_H

#include "driver/gpio.h"

// Board Info
#define BOARD_HAL_NAME "seeedstudio_reterminal_e1003"
#define BOARD_HAL_TYPE BOARD_TYPE_SEEEDSTUDIO_RETERMINAL_E1003

// Display color model reported to the server (selects the GC16 grayscale path).
#define BOARD_HAL_DISPLAY_TYPE "gc16"

// Button Definitions (active-low). Mirror the E1004 mapping:
//   Refresh (GPIO5) -> Wake, Next/right (GPIO3) -> Rotate, Prev/left (GPIO4) -> Clear
#define BOARD_HAL_WAKEUP_KEY GPIO_NUM_5  // Refresh button (Wake/Select)
#define BOARD_HAL_WAKEUP_KEY_NAME "Refresh Button"
#define BOARD_HAL_ROTATE_KEY GPIO_NUM_3  // Next / right button (Rotate)
#define BOARD_HAL_CLEAR_KEY GPIO_NUM_4   // Prev / left button (Clear)

// SPI Pins (shared by the IT8951 controller and the microSD card)
#define BOARD_HAL_SPI_SCLK_PIN GPIO_NUM_7
#define BOARD_HAL_SPI_MOSI_PIN GPIO_NUM_9
#define BOARD_HAL_SPI_MISO_PIN GPIO_NUM_8

// E-Paper: IT8951 T-CON (10.3" ED103TC2 grayscale, GC16). No DC line (the
// IT8951 uses an SPI preamble), no second CS. BUSY is the IT8951 HRDY pin.
#define BOARD_HAL_EPD_CS_PIN GPIO_NUM_10      // ITE_CS#
#define BOARD_HAL_EPD_DC_PIN (-1)             // unused on IT8951
#define BOARD_HAL_EPD_CS1_PIN (-1)            // unused (single panel)
#define BOARD_HAL_EPD_RST_PIN GPIO_NUM_12     // ITE_RST#
#define BOARD_HAL_EPD_BUSY_PIN GPIO_NUM_13    // ITE_BUSY# / HRDY
#define BOARD_HAL_EPD_ENABLE_PIN GPIO_NUM_11  // EPD_Drive_EN (TPS65185 bias rail)
#define BOARD_HAL_EPD_VCC_EN_PIN GPIO_NUM_21  // ITE_VCC_EN (IT8951 3V3/1V8 core)

// SD Card
#define BOARD_HAL_SD_PWR_PIN GPIO_NUM_39  // SD_EN load switch
#define BOARD_HAL_SD_CS_PIN GPIO_NUM_14

// I2C Pins (I2C0: RTC 0x51 + SHT40 0x44 + SY6974B charger 0x6B)
#define BOARD_HAL_I2C_SDA_PIN GPIO_NUM_19
#define BOARD_HAL_I2C_SCL_PIN GPIO_NUM_20

// Power Management
#define BOARD_HAL_BAT_ADC_PIN ADC_CHANNEL_0  // GPIO 1
#define BOARD_HAL_BAT_EN_PIN GPIO_NUM_40

// Onboard LED
#define BOARD_HAL_LED_PIN GPIO_NUM_16
#define BOARD_HAL_LED_INVERTED false

// Display Configuration (panel is native landscape 1872x1404; IT8951 reports it)
#define BOARD_HAL_DISPLAY_ROTATION_DEG 0

// The IT8951 and the microSD card share the SPI bus; auto light sleep isolates
// GPIOs mid-transaction and breaks SD reads (see E1004 note). Disable it here.
#define BOARD_HAL_DISABLE_AUTO_LIGHT_SLEEP 1

#endif  // BOARD_SEEEDSTUDIO_RETERMINAL_E1003_H
