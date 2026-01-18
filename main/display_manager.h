#ifndef DISPLAY_MANAGER_H
#define DISPLAY_MANAGER_H

#include <stdbool.h>

#include "esp_err.h"

esp_err_t display_manager_init(void);
esp_err_t display_manager_show_image(const char *filename);
esp_err_t display_manager_clear(void);
bool display_manager_is_busy(void);
void display_manager_rotate_from_sdcard(void);
const char *display_manager_get_current_image(void);
void display_manager_initialize_paint(void);

#endif
