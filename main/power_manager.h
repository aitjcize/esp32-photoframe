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

/**
 * @brief Override the auto-sleep timeout at runtime (seconds).
 *
 * Resets the running countdown so the new value takes effect immediately.
 * Used to extend the window during out-of-box provisioning.
 */
void power_manager_set_auto_sleep_timeout(uint32_t seconds);
void power_manager_reset_rotate_timer(void);

/**
 * @brief Seconds remaining until the boundary this timer wake was targeting.
 *
 * Based on the current (possibly corrected) system time. If an external RTC
 * restore or NTP sync pulled the clock backward, a positive value means the
 * timer fired early due to RTC drift and the scheduled rotation time hasn't
 * arrived yet. Returns 0 for non-timer wakes or once the boundary is reached.
 */
int power_manager_get_seconds_until_wake_target(void);
wakeup_source_t power_manager_get_wakeup_source(void);
void power_manager_set_deep_sleep_enabled(bool enabled);

#endif
