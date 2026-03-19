#ifndef BOARD_SEEEDSTUDIO_RETERMINAL_E1004_H
#define BOARD_SEEEDSTUDIO_RETERMINAL_E1004_H

#include "driver/gpio.h"

// Board Info
#define BOARD_HAL_NAME "seeedstudio_reterminal_e1004"
#define BOARD_HAL_TYPE BOARD_TYPE_SEEEDSTUDIO_RETERMINAL_E1004

// Button Definitions (physical keys)
#define BOARD_HAL_WAKEUP_KEY GPIO_NUM_5  // KEY2 (refresh)
#define BOARD_HAL_ROTATE_KEY GPIO_NUM_3  // KEY0 (page up)
#define BOARD_HAL_CLEAR_KEY GPIO_NUM_4   // KEY1 (page down)

// SPI Pins
#define BOARD_HAL_SPI_SCLK_PIN GPIO_NUM_7
#define BOARD_HAL_SPI_MOSI_PIN GPIO_NUM_9
#define BOARD_HAL_SPI_MISO_PIN GPIO_NUM_8

// E-Paper Display Pins (ED2208 13.3" Spectra 6, 60-pin FPC)
#define BOARD_HAL_EPD_CS_PIN    GPIO_NUM_10  // SCREEN_CS#
#define BOARD_HAL_EPD_DC_PIN    GPIO_NUM_11  // SCREEN_DC#
#define BOARD_HAL_EPD_ENABLE_PIN GPIO_NUM_12 // SCREEN_EN#
#define BOARD_HAL_EPD_BUSY_PIN  GPIO_NUM_13  // SCREEN_BUSY#
#define BOARD_HAL_EPD_RST_PIN   GPIO_NUM_38  // SCREEN_RST#

// SD Card Pins
#define BOARD_HAL_SD_CS_PIN     GPIO_NUM_14  // SD_CS
#define BOARD_HAL_SD_DET_PIN    GPIO_NUM_15  // SD_DET (card detect, active-low)
#define BOARD_HAL_SD_PWR_PIN    GPIO_NUM_16  // SD_EN (TPS22916 load switch)

// I2C Bus 0 — SY6974B charger, PCF8563M RTC, SHT40 sensor
#define BOARD_HAL_I2C_SDA_PIN   GPIO_NUM_19
#define BOARD_HAL_I2C_SCL_PIN   GPIO_NUM_20

// I2C Bus 1 — Touch buttons
#define BOARD_HAL_I2C1_SDA_PIN  GPIO_NUM_39
#define BOARD_HAL_I2C1_SCL_PIN  GPIO_NUM_40

// Battery ADC
#define BOARD_HAL_BAT_ADC_PIN   ADC_CHANNEL_0  // GPIO1
#define BOARD_HAL_BAT_EN_PIN    GPIO_NUM_21     // VBAT_EN (TPS22916 ADC enable)

// Power Management
#define BOARD_HAL_POWER_EN_PIN  GPIO_NUM_46     // System power enable
#define BOARD_HAL_SY_INT_PIN    GPIO_NUM_47     // SY6974B charger interrupt

// Buzzer
#define BOARD_HAL_BUZZER_PIN    GPIO_NUM_45

// Onboard Status LED (active-high)
#define BOARD_HAL_LED_PIN       GPIO_NUM_48
#define BOARD_HAL_LED_INVERTED  false

// Display Configuration
#define BOARD_HAL_DISPLAY_ROTATION_DEG 0

#endif  // BOARD_SEEEDSTUDIO_RETERMINAL_E1004_H
