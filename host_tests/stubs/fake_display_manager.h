// Test-side view of the fake display_manager: captures everything the image
// pipeline streams to the display so tests can inspect the final frame.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "display_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

// Drop any captured frame and adopt the current test_board_display_* dims.
void fake_display_reset(void);

// Captured frame state (valid after a begin/…/end cycle)
int fake_display_frame_width(void);
int fake_display_frame_height(void);
const uint8_t *fake_display_frame(void);  // RGB888, frame_width*frame_height*3
bool fake_display_was_shown(void);        // end_rgb_stream(show=true) seen
int fake_display_begin_count(void);

// Copies of the publish spec passed to end_rgb_stream ("" when absent)
const char *fake_display_pub_display_name(void);
const char *fake_display_pub_save_path(void);
const char *fake_display_pub_fallback_name(void);

#ifdef __cplusplus
}
#endif
