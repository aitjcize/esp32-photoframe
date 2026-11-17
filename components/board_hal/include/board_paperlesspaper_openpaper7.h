#ifndef BOARD_PAPERLESSPAPER_OPENPAPER7_H
#define BOARD_PAPERLESSPAPER_OPENPAPER7_H

#include "driver/gpio.h"
#include "hal/adc_types.h"

// paperlesspaper OpenPaper 7 (Paper 7, PCB EPAPER73_V2): ESP32-C6-MINI-1-N4
// (4MB flash, no PSRAM) driving a 7.3" Spectra 6 panel (GDEP073E01 / EL073TF1).
// Pin map from the V2 schematic and the vendor firmware
// (github.com/paperlesspaper/paperlesspaper-firmware, src/main.cpp).

// Board Info
#define BOARD_HAL_NAME "paperlesspaper_openpaper7"
#define BOARD_HAL_TYPE BOARD_TYPE_PAPERLESSPAPER_OPENPAPER7

// Buttons. The large button is wired to the chip's EN (reset) line, so a press
// is a cold boot, which comes up with WiFi and the web UI like a wake-key
// press does on the other boards. The small button sits on GPIO9, which is
// not an LP GPIO on the C6 and so cannot wake the chip from deep sleep; it
// works as the rotate key while the frame is awake. Holding it through a
// reset enters the ROM download mode (GPIO9 is the boot strap).
#define BOARD_HAL_WAKEUP_KEY GPIO_NUM_NC
#define BOARD_HAL_WAKEUP_KEY_NAME "Large (reset) Button"
#define BOARD_HAL_ROTATE_KEY GPIO_NUM_9
#define BOARD_HAL_CLEAR_KEY GPIO_NUM_NC

// SPI bus shared by the panel and the external W25Q128 flash
#define BOARD_HAL_SPI_SCLK_PIN GPIO_NUM_15
#define BOARD_HAL_SPI_MOSI_PIN GPIO_NUM_4
#define BOARD_HAL_SPI_MISO_PIN GPIO_NUM_5

// E-Paper Display Pins (Spectra 6). The panel rail is not switched on this
// revision.
#define BOARD_HAL_EPD_CS_PIN GPIO_NUM_20
#define BOARD_HAL_EPD_DC_PIN GPIO_NUM_19
#define BOARD_HAL_EPD_RST_PIN GPIO_NUM_1
#define BOARD_HAL_EPD_BUSY_PIN GPIO_NUM_18

// External 16MB SPI NOR flash (W25Q128JVSIQ), used as photo storage. Its CS
// has an external 10k pull-up, so it stays deselected through deep sleep.
#define BOARD_HAL_EXT_FLASH_CS_PIN GPIO_NUM_21

// I2C: FUSB303 USB-C controller (0x21) and KXTJ3 accelerometer (0x0E)
#define BOARD_HAL_I2C_SDA_PIN GPIO_NUM_6
#define BOARD_HAL_I2C_SCL_PIN GPIO_NUM_7

// Battery (4x NiMH): 510k/392k divider straight off the battery rail into
// GPIO2 (ADC1 channel 2), always connected
#define BOARD_HAL_BAT_ADC_CHANNEL ADC_CHANNEL_2

// BQ25172 NiMH charger (4 cells, ~0.44 A). CHG_EN drives the charger's TS
// (thermistor) pin, which a 10k resistor to ground otherwise holds at a normal
// temperature: left floating, charging runs; driven low, TS reads as a fault
// and charging stops. CHG_STAT (open drain, pulled up) is low while charging.
#define BOARD_HAL_CHG_EN_PIN GPIO_NUM_22
#define BOARD_HAL_CHG_STAT_PIN GPIO_NUM_23

// Onboard green LED (active-high)
#define BOARD_HAL_LED_PIN GPIO_NUM_14

// The panel and the photo storage flash share one SPI bus. Untested whether
// automatic light sleep leaves esp_flash transactions on SPI2 intact, so keep
// it off (frequency scaling still applies).
#define BOARD_HAL_DISABLE_AUTO_LIGHT_SLEEP

// Display Configuration
#define BOARD_HAL_DISPLAY_ROTATION_DEG 0

#endif  // BOARD_PAPERLESSPAPER_OPENPAPER7_H
