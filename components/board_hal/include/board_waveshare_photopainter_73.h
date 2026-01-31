#ifndef BOARD_WAVESHARE_PHOTOPAINTER_73_H
#define BOARD_WAVESHARE_PHOTOPAINTER_73_H

#include "driver/gpio.h"

// Board Info
#define BOARD_HAL_NAME "waveshare_photopainter_73"
#define BOARD_HAL_TYPE BOARD_TYPE_WAVESHARE_PHOTOPAINTER

// Button Definitions
#define BOARD_HAL_WAKEUP_KEY GPIO_NUM_0  // BOOT button
#define BOARD_HAL_ROTATE_KEY GPIO_NUM_4  // Usage: Wakeup + Rotate

// Display Configuration
#define BOARD_HAL_DISPLAY_ROTATION_DEG 180
#define BOARD_HAL_DISPLAY_WIDTH 800
#define BOARD_HAL_DISPLAY_HEIGHT 480

#endif  // BOARD_WAVESHARE_PHOTOPAINTER_73_H
