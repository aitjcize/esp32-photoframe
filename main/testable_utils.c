#include "testable_utils.h"

int is_rotation_due(time_t now, const rotation_config_t *config)
{
    if (config->last_rotation == 0) {
        return 1;  // Never rotated before
    }

    if (config->mode == AR_MODE_DAILY_H) {
        // Daily Mode: Always Anchored Synchronized by definition (86400s interval)
        struct tm tm_anchor;
        localtime_r(&now, &tm_anchor);
        tm_anchor.tm_hour = config->start_time_min / 60;
        tm_anchor.tm_min = config->start_time_min % 60;
        tm_anchor.tm_sec = 0;
        time_t current_slot_start = mktime(&tm_anchor);
        if (current_slot_start > now) {
            current_slot_start -= 86400;
        }

        struct tm tm_last;
        localtime_r(&config->last_rotation, &tm_last);
        tm_last.tm_hour = config->start_time_min / 60;
        tm_last.tm_min = config->start_time_min % 60;
        tm_last.tm_sec = 0;
        time_t last_slot_start = mktime(&tm_last);
        if (last_slot_start > config->last_rotation) {
            last_slot_start -= 86400;
        }

        if (current_slot_start <= last_slot_start) {
            return 0;  // Already rotated in current or future slot
        }

        return (int) ((current_slot_start - last_slot_start) / 86400);
    } else {
        // Interval Mode
        if (config->policy == AR_POLICY_SEQUENTIAL_H) {
            // Sequential: NEVER skip images.
            if (config->use_anchor) {
                // Anchored Sequential: rotate if we crossed a slot boundary since last
                struct tm tm_anchor;
                localtime_r(&now, &tm_anchor);
                tm_anchor.tm_hour = config->start_time_min / 60;
                tm_anchor.tm_min = config->start_time_min % 60;
                tm_anchor.tm_sec = 0;
                time_t day_anchor = mktime(&tm_anchor);
                if (day_anchor > now) {
                    day_anchor -= 86400;
                }

                long seconds_since_anchor = (long) (now - day_anchor);
                time_t current_slot_start =
                    day_anchor +
                    (seconds_since_anchor / config->interval_sec) * config->interval_sec;

                return (config->last_rotation < current_slot_start) ? 1 : 0;
            } else {
                // Relative Sequential: strictly interval based timer
                return (now - config->last_rotation) >= config->interval_sec ? 1 : 0;
            }
        } else {
            // Synchronized: catch up to fixed time slots (allowing skips)
            if (config->use_anchor) {
                // Anchored Synchronized: use fixed wall-clock slots
                struct tm tm_anchor;
                localtime_r(&now, &tm_anchor);
                tm_anchor.tm_hour = config->start_time_min / 60;
                tm_anchor.tm_min = config->start_time_min % 60;
                tm_anchor.tm_sec = 0;
                time_t day_anchor = mktime(&tm_anchor);
                if (day_anchor > now) {
                    day_anchor -= 86400;
                }

                long seconds_since_anchor = (long) (now - day_anchor);
                time_t current_slot_start =
                    day_anchor +
                    (seconds_since_anchor / config->interval_sec) * config->interval_sec;

                // Find last slot start similarly
                struct tm tm_last_anchor;
                localtime_r(&config->last_rotation, &tm_last_anchor);
                tm_last_anchor.tm_hour = config->start_time_min / 60;
                tm_last_anchor.tm_min = config->start_time_min % 60;
                tm_last_anchor.tm_sec = 0;
                time_t last_day_anchor = mktime(&tm_last_anchor);
                if (last_day_anchor > config->last_rotation) {
                    last_day_anchor -= 86400;
                }

                long last_seconds_since_anchor = (long) (config->last_rotation - last_day_anchor);
                time_t last_slot_start =
                    last_day_anchor +
                    (last_seconds_since_anchor / config->interval_sec) * config->interval_sec;

                if (current_slot_start <= last_slot_start) {
                    return 0;
                }

                return (int) ((current_slot_start - last_slot_start) / config->interval_sec);
            } else {
                // Relative Synchronized: stay in sync relative to last rotation time
                // skip = floor(time passed / interval)
                long passed = (long) (now - config->last_rotation);
                return (int) (passed / config->interval_sec);
            }
        }
    }
}

static bool is_in_sleep_schedule(int current_minutes, const sleep_schedule_config_t *sleep_schedule)
{
    if (!sleep_schedule || !sleep_schedule->enabled) {
        return false;
    }

    if (sleep_schedule->start_minutes > sleep_schedule->end_minutes) {
        // Overnight schedule
        return current_minutes >= sleep_schedule->start_minutes ||
               current_minutes < sleep_schedule->end_minutes;
    } else {
        // Same-day schedule
        return current_minutes >= sleep_schedule->start_minutes &&
               current_minutes < sleep_schedule->end_minutes;
    }
}

int calculate_next_wakeup_interval(time_t now, const rotation_config_t *rot_config,
                                   const sleep_schedule_config_t *sleep_schedule)
{
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    int current_min = tm_now.tm_hour * 60 + tm_now.tm_min;

    // 1. If we are currently in sleep schedule, we must wake up at or after sleep_end
    if (is_in_sleep_schedule(current_min, sleep_schedule)) {
        int minutes_until_end;
        if (current_min < sleep_schedule->end_minutes) {
            minutes_until_end = sleep_schedule->end_minutes - current_min;
        } else {
            minutes_until_end = (1440 - current_min) + sleep_schedule->end_minutes;
        }
        time_t end_of_sleep = now + (minutes_until_end * 60 - tm_now.tm_sec);

        if (!rot_config->use_anchor) {
            return (int) (end_of_sleep - now);
        }

        // For anchored modes, find the first slot >= end_of_sleep
        struct tm tm_end;
        localtime_r(&end_of_sleep, &tm_end);
        tm_end.tm_hour = rot_config->start_time_min / 60;
        tm_end.tm_min = rot_config->start_time_min % 60;
        tm_end.tm_sec = 0;
        time_t anchor_ts = mktime(&tm_end);
        if (anchor_ts > end_of_sleep) {
            anchor_ts -= 86400;
        }

        long seconds_since_anchor = (long) (end_of_sleep - anchor_ts);
        long intervals =
            (seconds_since_anchor + rot_config->interval_sec - 1) / rot_config->interval_sec;
        time_t first_slot = anchor_ts + (intervals * rot_config->interval_sec);
        return (int) (first_slot - now);
    }

    // 2. Calculate raw seconds until next rotation slot
    int seconds_until_rotation = 0;
    if (!rot_config->use_anchor) {
        // Relative mode: always a full interval
        seconds_until_rotation = rot_config->interval_sec;
    } else {
        // Anchored mode: align to wall clock slots
        struct tm tm_anchor = tm_now;
        tm_anchor.tm_hour = rot_config->start_time_min / 60;
        tm_anchor.tm_min = rot_config->start_time_min % 60;
        tm_anchor.tm_sec = 0;
        time_t anchor_ts = mktime(&tm_anchor);
        if (anchor_ts > now) {
            anchor_ts -= 86400;
        }

        long seconds_since_anchor = (long) (now - anchor_ts);
        long next_slot_ts = anchor_ts + (seconds_since_anchor / rot_config->interval_sec + 1) *
                                            rot_config->interval_sec;

        seconds_until_rotation = (int) (next_slot_ts - now);

        // Drift Protection: If we are too close to the current slot (e.g. woke up 10s early),
        // skip to the next one to avoid a tight wake loop.
        if (seconds_until_rotation < 60) {
            seconds_until_rotation += rot_config->interval_sec;
        }
    }

    // 3. Final check: Does our next rotation fall into the sleep schedule?
    time_t next_wakeup_ts = now + seconds_until_rotation;
    struct tm tm_next;
    localtime_r(&next_wakeup_ts, &tm_next);
    int next_wakeup_min = tm_next.tm_hour * 60 + tm_next.tm_min;

    if (is_in_sleep_schedule(next_wakeup_min, sleep_schedule)) {
        // Waking up at next_wakeup_ts would land us in sleep.
        // Calculate the end of that sleep schedule.
        int minutes_until_end;
        if (next_wakeup_min < sleep_schedule->end_minutes) {
            minutes_until_end = sleep_schedule->end_minutes - next_wakeup_min;
        } else {
            minutes_until_end = (1440 - next_wakeup_min) + sleep_schedule->end_minutes;
        }
        time_t end_of_sleep = next_wakeup_ts + (minutes_until_end * 60 - tm_next.tm_sec);

        if (!rot_config->use_anchor) {
            return (int) (end_of_sleep - now);
        }

        // Re-align to first slot >= end_of_sleep
        struct tm tm_end;
        localtime_r(&end_of_sleep, &tm_end);
        tm_end.tm_hour = rot_config->start_time_min / 60;
        tm_end.tm_min = rot_config->start_time_min % 60;
        tm_end.tm_sec = 0;
        time_t anchor_ts = mktime(&tm_end);
        if (anchor_ts > end_of_sleep) {
            anchor_ts -= 86400;
        }
        long seconds_since_anchor = (long) (end_of_sleep - anchor_ts);
        long intervals =
            (seconds_since_anchor + rot_config->interval_sec - 1) / rot_config->interval_sec;
        time_t first_slot = anchor_ts + (intervals * rot_config->interval_sec);
        return (int) (first_slot - now);
    }

    return seconds_until_rotation;
}
