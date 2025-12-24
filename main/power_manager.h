#ifndef POWER_MANAGER_H
#define POWER_MANAGER_H

#include <stdbool.h>

#include "esp_err.h"

esp_err_t power_manager_init(void);
void power_manager_enter_sleep(void);
void power_manager_enter_sleep_with_timer(uint32_t sleep_time_sec);
void power_manager_reset_sleep_timer(void);
void power_manager_reset_rotate_timer(void);
bool power_manager_is_timer_wakeup(void);
bool power_manager_is_ext1_wakeup(void);
bool power_manager_is_boot_button_wakeup(void);
bool power_manager_is_key_button_wakeup(void);
void power_manager_set_deep_sleep_enabled(bool enabled);
bool power_manager_get_deep_sleep_enabled(void);

#endif
