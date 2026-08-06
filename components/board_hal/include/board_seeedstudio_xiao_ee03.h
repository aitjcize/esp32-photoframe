#ifndef BOARD_SEEEDSTUDIO_XIAO_EE03_H
#define BOARD_SEEEDSTUDIO_XIAO_EE03_H

#include "driver/gpio.h"

// Board Info
#define BOARD_HAL_NAME "seeedstudio_xiao_ee03"
#define BOARD_HAL_TYPE BOARD_TYPE_SEEEDSTUDIO_XIAO_EE03

// Display color model reported to the server (selects the GC16 grayscale path).
#define BOARD_HAL_DISPLAY_TYPE "gc16"

// Buttons (active-low), same wiring as the XIAO EE02/EE04 boards.
#define BOARD_HAL_WAKEUP_KEY GPIO_NUM_5  // Button 3
#define BOARD_HAL_WAKEUP_KEY_NAME "Button 3"
#define BOARD_HAL_ROTATE_KEY GPIO_NUM_2  // Button 1
#define BOARD_HAL_CLEAR_KEY GPIO_NUM_3   // Button 2

// SPI Pins (IT8951 only; no microSD). MISO is wired (unlike EE02/EE04) because
// the IT8951 is read back over SPI.
#define BOARD_HAL_SPI_SCLK_PIN GPIO_NUM_7  // D8
#define BOARD_HAL_SPI_MOSI_PIN GPIO_NUM_9  // D10
#define BOARD_HAL_SPI_MISO_PIN GPIO_NUM_8  // D9

// E-Paper: IT8951 T-CON (10.3" ED103TC2 grayscale, GC16). No DC line (the
// IT8951 uses an SPI preamble), single CS. BUSY is the IT8951 HRDY pin.
#define BOARD_HAL_EPD_CS_PIN GPIO_NUM_44      // TFT_CS  (D7)
#define BOARD_HAL_EPD_DC_PIN (-1)             // unused on IT8951
#define BOARD_HAL_EPD_CS1_PIN (-1)            // unused (single panel)
#define BOARD_HAL_EPD_RST_PIN GPIO_NUM_38     // TFT_RST
#define BOARD_HAL_EPD_BUSY_PIN GPIO_NUM_4     // TFT_BUSY / HRDY
// Board-wide peripheral power enable (PWR_EN on the schematic): gates VCC_3V3
// (SHT40, font ROM, SPI flash), the IT8951 core rails, and the EPD bias --
// everything except the XIAO module. Driven high to power the subsystem; must
// be held low through deep sleep or it keeps draining the battery.
#define BOARD_HAL_PWR_EN_PIN GPIO_NUM_43  // PWR_EN: master peripheral power

// I2C Pins (SHT40 temperature/humidity sensor at 0x44).
#define BOARD_HAL_I2C_SCL_PIN GPIO_NUM_41  // RX1
#define BOARD_HAL_I2C_SDA_PIN GPIO_NUM_42  // TX1

// Onboard LED: the XIAO module's user LED on GPIO21, active-low (drive low =
// on). The board's other LEDs are charger/power indicators, not GPIO-driven.
#define BOARD_HAL_LED_PIN GPIO_NUM_21
#define BOARD_HAL_LED_INVERTED true

// Display Configuration (panel is native landscape 1872x1404; IT8951 reports it)
#define BOARD_HAL_DISPLAY_ROTATION_DEG 0

// The IT8951 image stream must not be interrupted by auto light sleep (it
// isolates GPIOs mid-transaction and corrupts the transfer). Matches E1003.
#define BOARD_HAL_DISABLE_AUTO_LIGHT_SLEEP 1

#endif  // BOARD_SEEEDSTUDIO_XIAO_EE03_H
