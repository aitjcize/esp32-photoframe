#ifndef DISPLAY_MANAGER_H
#define DISPLAY_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t display_manager_init(void);
esp_err_t display_manager_show_image(const char *filename);
esp_err_t display_manager_clear(void);
bool display_manager_is_busy(void);

void display_manager_set_rotate_interval(int seconds);
int display_manager_get_rotate_interval(void);
void display_manager_set_auto_rotate(bool enabled);
bool display_manager_get_auto_rotate(void);

/**
 * @brief Handle wakeup event and display appropriate image
 *
 * @param gallery_mode 0 = Local Gallery (random from enabled albums)
 *                     1 = Remote Gallery (display /sdcard/remote.bmp)
 *                     If remote.bmp doesn't exist, falls back to local mode.
 */
void display_manager_handle_wakeup(uint8_t gallery_mode);

#endif
