#ifndef TESTABLE_UTILS_H
#define TESTABLE_UTILS_H

#include <stdbool.h>
#include <time.h>

typedef enum { AR_MODE_INTERVAL = 0, AR_MODE_DAILY = 1 } ar_mode_t;

typedef enum { AR_POLICY_SYNCHRONIZED = 0, AR_POLICY_SEQUENTIAL = 1 } ar_policy_t;

typedef struct {
    bool enabled;
    int start_minutes;  // Minutes since midnight
    int end_minutes;    // Minutes since midnight
} sleep_schedule_config_t;

typedef struct {
    ar_mode_t mode;
    int interval_sec;
    int start_time_min;
    bool use_anchor;
    ar_policy_t policy;
    time_t last_rotation;
} rotation_config_t;

#ifdef __cplusplus
extern "C" {
#endif

// Check if a rotation is due based on current time and configuration.
// Returns the number of rotations that should be triggered (for catch-up).
int is_rotation_due(time_t now, const rotation_config_t *config);

// Calculate next wake-up interval considering rotation and sleep schedule
int calculate_next_wakeup_interval(time_t now, const rotation_config_t *rot_config,
                                   const sleep_schedule_config_t *sleep_schedule);

#ifdef __cplusplus
}
#endif

#endif
