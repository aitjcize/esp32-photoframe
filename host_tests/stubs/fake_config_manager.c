// Fake config_manager for host tests — only what image_processor.c links.
#include "config_manager.h"

display_orientation_t test_display_orientation = DISPLAY_ORIENTATION_LANDSCAPE;

display_orientation_t config_manager_get_display_orientation(void)
{
    return test_display_orientation;
}
