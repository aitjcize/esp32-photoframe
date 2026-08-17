// Fake processing settings for host tests — only what image_processor.c
// links; both knobs are test-controllable globals.
#include <stdio.h>

#include "processing_settings.h"

scale_mode_t test_scale_mode = SCALE_MODE_COVER;
const char *test_background_color = "white";

scale_mode_t processing_settings_get_scale_mode(void)
{
    return test_scale_mode;
}

void processing_settings_get_background_color(char *out, size_t out_size)
{
    snprintf(out, out_size, "%s", test_background_color);
}
