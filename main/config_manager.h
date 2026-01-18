#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <stdbool.h>

#include "config.h"
#include "esp_err.h"

esp_err_t config_manager_init(void);

void config_manager_set_rotate_interval(int seconds);
int config_manager_get_rotate_interval(void);

void config_manager_set_auto_rotate(bool enabled);
bool config_manager_get_auto_rotate(void);

void config_manager_set_image_orientation(int orientation);
int config_manager_get_image_orientation(void);

void config_manager_set_image_url(const char *url);
const char *config_manager_get_image_url(void);

void config_manager_set_rotation_mode(rotation_mode_t mode);
rotation_mode_t config_manager_get_rotation_mode(void);

void config_manager_set_save_downloaded_images(bool enabled);
bool config_manager_get_save_downloaded_images(void);

void config_manager_set_ha_url(const char *url);
const char *config_manager_get_ha_url(void);

#endif
