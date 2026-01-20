#include "testable_utils.h"

#include <stdbool.h>
#include <time.h>

// Forward declarations for config_manager functions (will be mocked in tests)
bool config_manager_get_sleep_schedule_enabled(void);
int config_manager_get_sleep_schedule_start(void);
int config_manager_get_sleep_schedule_end(void);

int calculate_next_aligned_wakeup(int rotate_interval)
{
    time_t now_time;
    struct tm timeinfo;
    time(&now_time);
    localtime_r(&now_time, &timeinfo);

    int current_seconds = timeinfo.tm_hour * 3600 + timeinfo.tm_min * 60 + timeinfo.tm_sec;
    int next_aligned_seconds = ((current_seconds / rotate_interval) + 1) * rotate_interval;
    int seconds_until_next = next_aligned_seconds - current_seconds;

    // Check if sleep schedule is enabled
    if (!config_manager_get_sleep_schedule_enabled()) {
        return seconds_until_next;
    }

    // Calculate the wake-up time in minutes since midnight
    int current_minutes = timeinfo.tm_hour * 60 + timeinfo.tm_min;
    int wake_minutes = current_minutes + (seconds_until_next / 60);

    // Normalize wake_minutes to handle day overflow
    if (wake_minutes >= 1440) {
        wake_minutes -= 1440;
    }

    int sleep_start = config_manager_get_sleep_schedule_start();
    int sleep_end = config_manager_get_sleep_schedule_end();

    // Check if wake-up time falls within or at the start of sleep schedule
    bool wake_in_schedule = false;
    if (sleep_start > sleep_end) {
        // Schedule crosses midnight (e.g., 23:00 - 07:00)
        // Wake is in schedule if >= start OR < end
        wake_in_schedule = (wake_minutes >= sleep_start || wake_minutes < sleep_end);
    } else {
        // Schedule within same day (e.g., 12:00 - 14:00)
        // Wake is in schedule if >= start AND < end
        wake_in_schedule = (wake_minutes >= sleep_start && wake_minutes < sleep_end);
    }

    if (!wake_in_schedule) {
        // Wake-up is outside sleep schedule, use normal interval
        return seconds_until_next;
    }

    // Wake-up would be in sleep schedule, calculate next aligned time at or after schedule ends
    int interval_minutes = rotate_interval / 60;

    // Find the first aligned time >= sleep_end (sleep_end is exclusive, so we can wake AT
    // sleep_end) This finds the first interval boundary that is >= sleep_end
    int next_aligned_wake =
        ((sleep_end + interval_minutes - 1) / interval_minutes) * interval_minutes;

    // Calculate seconds from current time to next aligned wake-up
    int minutes_until_wake;
    if (sleep_start > sleep_end) {
        // Overnight schedule (e.g., 23:00 - 07:00)
        if (current_minutes >= sleep_start || current_minutes < sleep_end) {
            // Currently in the schedule (either before midnight or after midnight)
            if (current_minutes >= sleep_start) {
                // Before midnight (e.g., 23:30) - wake after schedule ends next day
                minutes_until_wake = (1440 - current_minutes) + next_aligned_wake;
            } else {
                // After midnight (e.g., 02:00) - wake at next aligned time today
                minutes_until_wake = next_aligned_wake - current_minutes;
            }
        } else {
            // Currently between schedule end and start (e.g., 08:00-22:59)
            // But wake time is in schedule, so skip to next day's aligned time >= sleep_end
            minutes_until_wake = (1440 - current_minutes) + next_aligned_wake;
        }
    } else {
        // Same-day schedule (e.g., 12:00 - 14:00)
        // Wake time is in schedule, skip to first aligned time >= sleep_end
        minutes_until_wake = next_aligned_wake - current_minutes;
    }

    int adjusted_seconds = minutes_until_wake * 60;
    return adjusted_seconds;
}
