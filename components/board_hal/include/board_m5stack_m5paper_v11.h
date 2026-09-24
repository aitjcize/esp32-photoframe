#ifndef BOARD_M5STACK_M5PAPER_V11_H
#define BOARD_M5STACK_M5PAPER_V11_H

#include "driver/gpio.h"

// Board Info
#define BOARD_HAL_NAME "m5stack_m5paper_v11"
#define BOARD_HAL_TYPE BOARD_TYPE_M5STACK_M5PAPER_V11

// Display color model reported to the server (selects the GC16 grayscale path).
#define BOARD_HAL_DISPLAY_TYPE "gc16"

// Button Definitions (active-low, external pull-ups). The three side buttons are
// on GPIO37/38/39, which on the ESP32 are input-only pads with no internal
// pull-up/pull-down and no output driver — see BOARD_HAL_BUTTONS_NO_INTERNAL_PULL.
//   Push/centre (GPIO38) -> Wake, Left (GPIO37) -> Rotate, Right (GPIO39) -> Clear
#define BOARD_HAL_WAKEUP_KEY GPIO_NUM_38  // Wake / Select
#define BOARD_HAL_WAKEUP_KEY_NAME "Centre Button"
#define BOARD_HAL_ROTATE_KEY GPIO_NUM_37  // Rotate
#define BOARD_HAL_CLEAR_KEY GPIO_NUM_39   // Clear

// GPIO34-39 on the ESP32 are input-only: they have no internal pull resistors
// and cannot be latched with gpio_hold_en(). power_manager skips both for the
// button pins when this is defined. The board provides external pull-ups.
#define BOARD_HAL_BUTTONS_NO_INTERNAL_PULL 1

// The ESP32's EXT1 wake unit can only match "all pins low" or "any pin high",
// so it cannot wake on any-of-N active-low buttons the way the S3 can. The wake
// key is routed to EXT0 (single pin, level-triggered low) and the rotate key
// gets EXT1 as a one-pin ALL_LOW mask; the clear key is not a wake source and
// only works while the device is awake.
// Using EXT0 keeps the RTC peripheral power domain on through deep sleep, which
// costs some idle current. That is the price of waking on a button at all here,
// and it is small next to this board's own regulator quiescent draw.
#define BOARD_HAL_WAKEUP_KEY_USE_EXT0 1
#define BOARD_HAL_EXT1_KEYS_ARE_ALL_LOW 1

// SPI Pins (shared by the IT8951 controller and the microSD card)
#define BOARD_HAL_SPI_SCLK_PIN GPIO_NUM_14
#define BOARD_HAL_SPI_MOSI_PIN GPIO_NUM_12
#define BOARD_HAL_SPI_MISO_PIN GPIO_NUM_13

// E-Paper: IT8951 T-CON (4.7" ED047TC2 grayscale, GC16). No DC line (the IT8951
// uses an SPI preamble) and no second CS. BUSY is the IT8951 HRDY pin.
//
// There is no reset line on this board: the IT8951 is reset by cycling its
// supply rail (BOARD_HAL_EPD_ENABLE_PIN), which is what M5Stack's own driver
// does. The e-paper driver skips its reset pulse when pin_rst is negative.
#define BOARD_HAL_EPD_CS_PIN GPIO_NUM_15
#define BOARD_HAL_EPD_DC_PIN (-1)             // unused on IT8951
#define BOARD_HAL_EPD_CS1_PIN (-1)            // unused (single panel)
#define BOARD_HAL_EPD_RST_PIN (-1)            // no reset line; power-cycle instead
#define BOARD_HAL_EPD_BUSY_PIN GPIO_NUM_27    // HRDY
#define BOARD_HAL_EPD_ENABLE_PIN GPIO_NUM_23  // EPD_PWR_EN (IT8951 + panel bias)

// GetSystemInfo should report the panel geometry, but seed the fallback with the
// real 4.7" values instead of the driver's 10.3" default so a flaky first read
// can't leave the framebuffer sized for the wrong panel.
#define BOARD_HAL_EPD_PANEL_WIDTH 960
#define BOARD_HAL_EPD_PANEL_HEIGHT 540

// M5Stack's driver writes VCOM explicitly rather than trusting the value in the
// IT8951's waveform flash. -2.30 V is the value M5Stack ships for this panel.
#define BOARD_HAL_EPD_VCOM_MV 2300

// SD Card (no power switch — the slot is on the always-on 3V3 rail)
#define BOARD_HAL_SD_PWR_PIN (-1)
#define BOARD_HAL_SD_CS_PIN GPIO_NUM_4

// I2C Pins (I2C0: BM8563 RTC 0x51 + SHT30 0x44; GT911 touch 0x14 is unused)
#define BOARD_HAL_I2C_SDA_PIN GPIO_NUM_21
#define BOARD_HAL_I2C_SCL_PIN GPIO_NUM_22

// Power Management
// Battery sits behind a fixed 2:1 divider on GPIO35 (ADC1_CH7) with no gating
// load switch, so there is no BAT_EN pin.
#define BOARD_HAL_BAT_ADC_PIN ADC_CHANNEL_7  // GPIO 35
#define BOARD_HAL_BAT_EN_PIN (-1)

// Main power latch. The side power button only pulses the regulator on; the
// firmware must drive GPIO2 high to hold it, or the board powers itself off a
// moment after boot when running from the battery. It is latched through deep
// sleep with gpio_hold_en().
#define BOARD_HAL_MAIN_PWR_PIN GPIO_NUM_2

// Grove/external port power switch. Left off — nothing the firmware uses hangs
// off the external ports, and it is a measurable idle drain.
#define BOARD_HAL_EXT_PWR_EN_PIN GPIO_NUM_5

// No user-controllable LED on this board.
#define BOARD_HAL_LED_PIN (-1)

// Display Configuration. The panel is native landscape 960x540 and the IT8951
// reports it that way; the M5Paper's enclosure is portrait, but orientation is a
// user setting, so keep the panel's own orientation here like the other boards.
#define BOARD_HAL_DISPLAY_ROTATION_DEG 0

// The IT8951 and the microSD card share the SPI bus; auto light sleep isolates
// GPIOs mid-transaction and breaks SD reads (same as the E1003). Disable it.
#define BOARD_HAL_DISABLE_AUTO_LIGHT_SLEEP 1

#endif  // BOARD_M5STACK_M5PAPER_V11_H
