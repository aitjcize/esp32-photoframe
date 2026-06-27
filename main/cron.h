#ifndef CRON_H
#define CRON_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

// Quiet-hours window applied as a mask on top of the cron schedule: a matching
// rotation time that falls inside [start_minutes, end_minutes) is suppressed.
typedef struct {
    bool enabled;
    int start_minutes;  // Minutes since midnight
    int end_minutes;    // Minutes since midnight
} sleep_schedule_config_t;

// ============================================================================
// Cron-based rotation schedule engine (pure, host-testable)
//
// Uses a simplified 3-field cron expression: "minute hour day-of-week".
//   minute       0-59
//   hour         0-23
//   day-of-week  0-6  (0 = Sunday; 7 accepted as alias for Sunday)
//
// Per-field syntax: '*', 'a', 'a-b', '*/n', 'a-b/n', and comma lists of those.
// (Day-of-month and month are intentionally omitted — a photo frame never needs
// monthly/seasonal scheduling, and dropping them removes the Vixie dom/dow rule.)
// ============================================================================

#define CRON_MAX_HORIZON_SEC (8 * 24 * 3600)  // give up after 8 days of scanning
#define CRON_FALLBACK_SEC 3600                // returned when nothing matches in horizon
#define CRON_MIN_DELAY_SEC 60                 // never schedule a wake-up sooner than this

typedef struct {
    uint64_t minute;  // bits 0..59
    uint32_t hour;    // bits 0..23
    uint8_t dow;      // bits 0..6  (Sunday = 0)
} cron_rule_t;

#ifdef __cplusplus
extern "C" {
#endif

// Parse a single cron expression into a compiled rule.
// Returns true on success, false if the expression is malformed or out of range.
bool cron_parse(const char *expr, cron_rule_t *out);

// Does the wall-clock time described by `t` satisfy rule `r`?
// Uses tm_min, tm_hour and tm_wday (0=Sunday).
bool cron_match(const cron_rule_t *r, const struct tm *t);

// Seconds from `now` until the next minute that matches any of the `rules`,
// skipping any match that falls inside the quiet-hours window (`sleep` may be
// NULL or disabled). Honors a minimum delay of CRON_MIN_DELAY_SEC. Returns
// CRON_FALLBACK_SEC if no match is found within CRON_MAX_HORIZON_SEC.
int cron_seconds_until_next(const struct tm *now, const cron_rule_t *rules, int n_rules,
                            const sleep_schedule_config_t *sleep);

#ifdef __cplusplus
}
#endif

#endif  // CRON_H
