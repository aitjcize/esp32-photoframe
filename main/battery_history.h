#ifndef BATTERY_HISTORY_H
#define BATTERY_HISTORY_H

#include <stdbool.h>

#include "cJSON.h"

// Records the current battery reading to the persisted history log. No-op
// if no battery is present (e.g. pure USB power with no battery installed)
// or no persistent storage is mounted. Call once per successfully displayed
// image (see the history_manager_mark_shown() call site in
// display_manager.c's display_manager_show_image()).
//
// Automatically clears the whole history and starts a fresh log when the
// battery is freshly charged (>= BATTERY_HISTORY_RESET_PERCENT) or the
// oldest entry is more than BATTERY_HISTORY_MAX_AGE_DAYS old - see config.h.
void battery_history_record(void);

// Builds the JSON payload for the Web UI battery-history tab:
// {"entries": [{"t": <unix_ts>, "p": <0-100>, "c": <bool charging>}, ...],
//  "days_remaining": <number or null>}
// Caller owns the returned object (cJSON_Delete when done). Returns NULL on
// allocation failure.
cJSON *battery_history_build_json(void);

// Estimates days remaining until the battery reaches
// BATTERY_HISTORY_TARGET_PERCENT, based on the drain rate observed over the
// most recent battery-only (non-charging) run in the log. Returns false if
// there isn't enough such history yet to estimate (fresh log, no
// measurable drain, or currently charging with no prior battery-only data).
bool battery_history_estimate_days_remaining(double *out_days);

#endif
