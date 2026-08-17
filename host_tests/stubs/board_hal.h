// Host-test stub for board_hal.h — panel dimensions are test-controllable
// globals so rotation/scaling behavior can be exercised for any panel shape.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern int test_board_display_width;
extern int test_board_display_height;
extern const char *test_board_display_type;  // "spectra6" or "gc16"

#define BOARD_HAL_DISPLAY_WIDTH test_board_display_width
#define BOARD_HAL_DISPLAY_HEIGHT test_board_display_height
#define BOARD_HAL_DISPLAY_TYPE test_board_display_type

#ifdef __cplusplus
}
#endif
