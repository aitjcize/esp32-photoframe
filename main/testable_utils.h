#ifndef TESTABLE_UTILS_H
#define TESTABLE_UTILS_H

#include <stdbool.h>
#include <time.h>

typedef struct {
    bool enabled;
    int start_minutes;  // Minutes since midnight
    int end_minutes;    // Minutes since midnight
} sleep_schedule_config_t;

#ifdef __cplusplus
extern "C" {
#endif

// Calculate next wake-up interval considering sleep schedule
// Returns seconds until next wake-up
// Takes into account:
// - Current time (via timeinfo)
// - Clock alignment (if aligned=true, aligns to rotation interval boundaries)
// - Sleep schedule (skips wake-ups that fall within sleep schedule)
// - Overnight schedules (handles schedules that cross midnight)
// Note: rotations never run before their boundary (the caller re-sleeps on
// early wakes, see EARLY_WAKE_TOLERANCE_SEC), so an imminent boundary here
// is always a legitimate wake-up target.
int calculate_next_wakeup_interval(const struct tm *timeinfo, int rotate_interval, bool aligned,
                                   const sleep_schedule_config_t *sleep_schedule);

#ifdef __cplusplus
}
#endif

#endif
