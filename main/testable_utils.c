#include "testable_utils.h"

int calculate_next_wakeup_interval(const struct tm *timeinfo, int rotate_interval, bool aligned,
                                   const sleep_schedule_config_t *sleep_schedule)
{
    int current_seconds_of_day =
        timeinfo->tm_hour * 3600 + timeinfo->tm_min * 60 + timeinfo->tm_sec;
    int seconds_until_next;

    if (aligned) {
        int next_aligned_seconds =
            ((current_seconds_of_day / rotate_interval) + 1) * rotate_interval;
        seconds_until_next = next_aligned_seconds - current_seconds_of_day;

        // If next wakeup is too soon (less than 60s), skip to the following interval.
        // This prevents immediate re-wakeup due to time drift.
        if (seconds_until_next < 60) {
            next_aligned_seconds += rotate_interval;
            seconds_until_next = next_aligned_seconds - current_seconds_of_day;
        }
    } else {
        seconds_until_next = rotate_interval;
    }

    // Check if sleep schedule is enabled
    if (sleep_schedule == NULL || !sleep_schedule->enabled) {
        return seconds_until_next;
    }

    // Calculate the wake-up time in seconds since midnight
    int wake_seconds_of_day = current_seconds_of_day + seconds_until_next;

    // Normalize wake_seconds_of_day to handle day overflow
    while (wake_seconds_of_day >= 86400) {
        wake_seconds_of_day -= 86400;
    }

    int sleep_start_seconds = sleep_schedule->start_minutes * 60;
    int sleep_end_seconds = sleep_schedule->end_minutes * 60;

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

    // Wake-up would be in sleep schedule, calculate next wake-up time at or after schedule ends.
    long long next_wake_seconds_of_day;
    if (aligned) {
        // Find the first aligned time >= sleep_end (sleep_end is exclusive).
        next_wake_seconds_of_day = ((long long) sleep_end_seconds + rotate_interval - 1) /
                                   rotate_interval * rotate_interval;
    } else {
        // For non-aligned rotation, just wake up exactly when the sleep schedule ends
        next_wake_seconds_of_day = sleep_end_seconds;
    }

    // Calculate seconds from current time to next wake-up
    int seconds_until_wake;
    if (sleep_start_seconds > sleep_end_seconds) {
        // Overnight schedule (e.g., 23:00 - 07:00)
        if (current_seconds_of_day >= sleep_start_seconds ||
            current_seconds_of_day < sleep_end_seconds) {
            // Currently in the schedule
            if (current_seconds_of_day >= sleep_start_seconds) {
                // Before midnight - wake after schedule ends next day
                seconds_until_wake =
                    (86400 - current_seconds_of_day) + (int) next_wake_seconds_of_day;
            } else {
                // After midnight - wake at next aligned time today
                seconds_until_wake = (int) next_wake_seconds_of_day - current_seconds_of_day;
            }
        } else {
            // Currently between schedule end and start
            // But wake time is in schedule, so skip to next day
            seconds_until_wake = (86400 - current_seconds_of_day) + (int) next_wake_seconds_of_day;
        }
    } else {
        // Same-day schedule
        seconds_until_wake = (int) next_wake_seconds_of_day - current_seconds_of_day;
        if (seconds_until_wake < 0) {
            seconds_until_wake += 86400;  // Wrap to next day
        }
    }

    return seconds_until_wake;
}
