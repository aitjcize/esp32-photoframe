#ifndef BOARD_SEEEDSTUDIO_RETERMINAL_E1002_H
#define BOARD_SEEEDSTUDIO_RETERMINAL_E1002_H

#include "driver/gpio.h"

// Board Info
#define BOARD_HAL_NAME "seeedstudio_reterminal_e1002"
#define BOARD_HAL_TYPE BOARD_TYPE_SEEEDSTUDIO_RETERMINAL_E1002

// Button Definitions
#define BOARD_HAL_WAKEUP_KEY GPIO_NUM_3  // Green Button (Wake/Select)
#define BOARD_HAL_WAKEUP_KEY_NAME "Green Refresh Button"
#define BOARD_HAL_ROTATE_KEY GPIO_NUM_5  // Left Button (Rotate)
#define BOARD_HAL_CLEAR_KEY GPIO_NUM_4   // Right Button (Clear)

// SPI Pins
#define BOARD_HAL_SPI_SCLK_PIN GPIO_NUM_7
#define BOARD_HAL_SPI_MOSI_PIN GPIO_NUM_9
#define BOARD_HAL_SPI_MISO_PIN GPIO_NUM_8

// E-Paper Display Pins (Spectra E6)
#define BOARD_HAL_EPD_DC_PIN GPIO_NUM_11
#define BOARD_HAL_EPD_CS_PIN GPIO_NUM_10
#define BOARD_HAL_EPD_RST_PIN GPIO_NUM_12
#define BOARD_HAL_EPD_BUSY_PIN GPIO_NUM_13

// SD Card Pins
#define BOARD_HAL_SD_PWR_PIN GPIO_NUM_16
#define BOARD_HAL_SD_CS_PIN GPIO_NUM_14

// I2C Pins (I2C0: RTC + T/RH sensor)
#define BOARD_HAL_I2C_SDA_PIN GPIO_NUM_19
#define BOARD_HAL_I2C_SCL_PIN GPIO_NUM_20

// SY6974B charger on a separate I2C1 bus (present on V1.2+; earlier revisions
// ship a non-I2C ETA6003 charger, so the runtime probe handles its absence).
#define BOARD_HAL_CHARGER_I2C_SDA_PIN GPIO_NUM_39
#define BOARD_HAL_CHARGER_I2C_SCL_PIN GPIO_NUM_40

// Power Management
#define BOARD_HAL_BAT_ADC_PIN ADC_CHANNEL_0  // GPIO 1
#define BOARD_HAL_BAT_EN_PIN GPIO_NUM_21

// Onboard LED (active-low)
#define BOARD_HAL_LED_PIN GPIO_NUM_6
#define BOARD_HAL_LED_INVERTED true

// Display Configuration
#define BOARD_HAL_DISPLAY_ROTATION_DEG 0

#endif  // BOARD_SEEEDSTUDIO_RETERMINAL_E1002_H
