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

    // If next wakeup is too soon (less than 60s), skip to the following interval.
    // This prevents immediate re-wakeup due to time drift (e.g., waking at 16:59:20 instead of
    // 17:00:00). With daily NTP sync, drift should be well under 60s.
    if (seconds_until_next < 60) {
        next_aligned_seconds += rotate_interval;
        seconds_until_next = next_aligned_seconds - current_seconds;
    }

    // Check if sleep schedule is enabled
    if (!config_manager_get_sleep_schedule_enabled()) {
        return seconds_until_next;
    }

    // Calculate the wake-up time in seconds since midnight
    int current_seconds_of_day = timeinfo.tm_hour * 3600 + timeinfo.tm_min * 60 + timeinfo.tm_sec;
    int wake_seconds_of_day = current_seconds_of_day + seconds_until_next;

    // Normalize wake_seconds_of_day to handle day overflow
    if (wake_seconds_of_day >= 86400) {
        wake_seconds_of_day -= 86400;
    }

    int sleep_start_seconds = config_manager_get_sleep_schedule_start() * 60;
    int sleep_end_seconds = config_manager_get_sleep_schedule_end() * 60;

    // Check if wake-up time falls within or at the start of sleep schedule
    bool wake_in_schedule = false;
    if (sleep_start_seconds > sleep_end_seconds) {
        // Schedule crosses midnight (e.g., 23:00 - 07:00)
        // Wake is in schedule if >= start OR < end
        wake_in_schedule =
            (wake_seconds_of_day >= sleep_start_seconds || wake_seconds_of_day < sleep_end_seconds);
    } else {
        // Schedule within same day (e.g., 12:00 - 14:00)
        // Wake is in schedule if >= start AND < end
        wake_in_schedule =
            (wake_seconds_of_day >= sleep_start_seconds && wake_seconds_of_day < sleep_end_seconds);
    }

    if (!wake_in_schedule) {
        // Wake-up is outside sleep schedule, use normal interval
        return seconds_until_next;
    }

    // Wake-up would be in sleep schedule, calculate next aligned time at or after schedule ends.
    // Find the first aligned time >= sleep_end (sleep_end is exclusive).

    long long next_aligned_wake_seconds =
        ((long long) sleep_end_seconds + rotate_interval - 1) / rotate_interval * rotate_interval;

    // Calculate seconds from current time to next aligned wake-up
    int seconds_until_wake;
    if (sleep_start_seconds > sleep_end_seconds) {
        // Overnight schedule (e.g., 23:00 - 07:00)
        if (current_seconds_of_day >= sleep_start_seconds ||
            current_seconds_of_day < sleep_end_seconds) {
            // Currently in the schedule
            if (current_seconds_of_day >= sleep_start_seconds) {
                // Before midnight - wake after schedule ends next day
                seconds_until_wake =
                    (86400 - current_seconds_of_day) + (int) next_aligned_wake_seconds;
            } else {
                // After midnight - wake at next aligned time today
                seconds_until_wake = (int) next_aligned_wake_seconds - current_seconds_of_day;
            }
        } else {
            // Currently between schedule end and start
            // But wake time is in schedule, so skip to next day
            seconds_until_wake = (86400 - current_seconds_of_day) + (int) next_aligned_wake_seconds;
        }
    } else {
        // Same-day schedule
        seconds_until_wake = (int) next_aligned_wake_seconds - current_seconds_of_day;
    }

    return seconds_until_wake;
}
