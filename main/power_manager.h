#ifndef POWER_MANAGER_H
#define POWER_MANAGER_H

#include <stdbool.h>

#include "esp_err.h"

typedef enum {
    WAKEUP_SOURCE_NONE,           // Not a deep sleep wakeup (cold boot, reset, etc.)
    WAKEUP_SOURCE_TIMER,          // Timer wakeup (auto-rotate)
    WAKEUP_SOURCE_BOOT_BUTTON,    // BOOT/WAKEUP button pressed
    WAKEUP_SOURCE_ROTATE_BUTTON,  // ROTATE button pressed
    WAKEUP_SOURCE_CLEAR_BUTTON,   // CLEAR button pressed
    WAKEUP_SOURCE_EXT1_UNKNOWN,   // EXT1 wakeup from unknown GPIO
} wakeup_source_t;

esp_err_t power_manager_init(void);
void power_manager_enter_sleep(void);
void power_manager_enter_sleep_with_timer(uint32_t sleep_time_sec);
void power_manager_reset_sleep_timer(void);
void power_manager_reset_rotate_timer(void);
wakeup_source_t power_manager_get_wakeup_source(void);
void power_manager_set_deep_sleep_enabled(bool enabled);

// Poll the wake key for up to ~5s after a BOOT_BUTTON wakeup. Returns
// true if the user kept the button held the entire time, signalling
// that this boot should run in emergency-AP mode (regardless of the
// persistent wifi_mode NVS setting). Safe to call when the wakeup
// source is anything else — returns false immediately.
//
// Activity LED is pulsed at the 5s mark so the user knows when to
// release.
bool power_manager_check_long_press_for_ap(void);

#endif
