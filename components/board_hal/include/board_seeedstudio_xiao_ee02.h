#ifndef BOARD_SEEEDSTUDIO_XIAO_EE02_H
#define BOARD_SEEEDSTUDIO_XIAO_EE02_H

#include "driver/gpio.h"

// Board Info
#define BOARD_HAL_NAME "seeedstudio_xiao_ee02"
#define BOARD_HAL_TYPE BOARD_TYPE_SEEEDSTUDIO_XIAO_EE02

// Button Definitions
#define BOARD_HAL_WAKEUP_KEY GPIO_NUM_2  // Button 1
#define BOARD_HAL_ROTATE_KEY GPIO_NUM_3  // Button 2

// Display Configuration
#define BOARD_HAL_DISPLAY_ROTATION_DEG 0
#define BOARD_HAL_DISPLAY_WIDTH 1200
#define BOARD_HAL_DISPLAY_HEIGHT 1600

#endif  // BOARD_SEEEDSTUDIO_XIAO_EE02_H
