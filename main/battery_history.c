#include "battery_history.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "board_hal.h"
#include "config.h"
#include "esp_log.h"
#include "storage.h"

static const char *TAG = "battery_history";

// Same "-1 means unknown, never a real low reading" guard as
// telegram_bot.c's get_valid_battery_percent() - kept as a local duplicate
// (small, and this module has no other reason to depend on telegram_bot.c).
static bool get_valid_battery_reading(int *out_percent, bool *out_charging)
{
    if (!board_hal_is_battery_connected()) {
        return false;
    }
    int percent = board_hal_get_battery_percent();
    if (percent < 0) {
        return false;
    }
    *out_percent = percent;
    *out_charging = board_hal_is_charging() || board_hal_is_usb_connected();
    return true;
}

// Peeks the timestamp of the first (oldest) line in the history file.
static bool peek_oldest_timestamp(time_t *out_ts)
{
    FILE *f = fopen(BATTERY_HISTORY_PATH, "r");
    if (!f) {
        return false;
    }
    char line[64];
    bool found = false;
    if (fgets(line, sizeof(line), f)) {
        long long ts = 0;
        if (sscanf(line, "%lld,", &ts) == 1) {
            *out_ts = (time_t) ts;
            found = true;
        }
    }
    fclose(f);
    return found;
}

void battery_history_record(void)
{
    int percent;
    bool charging;
    if (!get_valid_battery_reading(&percent, &charging)) {
        return;  // No battery installed - nothing meaningful to log.
    }
    if (!storage_has_persistent_storage()) {
        return;  // No persistent storage - can't keep a history file.
    }

    // Reset conditions: fresh full charge starts a new discharge cycle, or
    // the log has simply gotten too old (e.g. permanently on USB, never
    // reaching the reset-percent trigger).
    bool should_reset = (percent >= BATTERY_HISTORY_RESET_PERCENT);
    const char *reset_reason = "fresh charge";
    if (!should_reset) {
        time_t oldest;
        if (peek_oldest_timestamp(&oldest)) {
            double age_days = difftime(time(NULL), oldest) / 86400.0;
            if (age_days > BATTERY_HISTORY_MAX_AGE_DAYS) {
                should_reset = true;
                reset_reason = "log too old";
            }
        }
    }
    if (should_reset) {
        remove(BATTERY_HISTORY_PATH);
        ESP_LOGI(TAG, "Battery history reset (%s)", reset_reason);
    }

    FILE *f = fopen(BATTERY_HISTORY_PATH, "a");
    if (!f) {
        ESP_LOGW(TAG, "Failed to open battery history for append");
        return;
    }
    fprintf(f, "%lld,%d,%d\n", (long long) time(NULL), percent, charging ? 1 : 0);
    fclose(f);
}

cJSON *battery_history_build_json(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    cJSON *entries = cJSON_AddArrayToObject(root, "entries");
    if (!entries) {
        cJSON_Delete(root);
        return NULL;
    }

    if (storage_has_persistent_storage()) {
        FILE *f = fopen(BATTERY_HISTORY_PATH, "r");
        if (f) {
            char line[64];
            while (fgets(line, sizeof(line), f)) {
                long long ts = 0;
                int percent = 0, charging = 0;
                if (sscanf(line, "%lld,%d,%d", &ts, &percent, &charging) == 3) {
                    cJSON *e = cJSON_CreateObject();
                    if (e) {
                        cJSON_AddNumberToObject(e, "t", (double) ts);
                        cJSON_AddNumberToObject(e, "p", percent);
                        cJSON_AddBoolToObject(e, "c", charging != 0);
                        cJSON_AddItemToArray(entries, e);
                    }
                }
            }
            fclose(f);
        }
    }

    double days;
    if (battery_history_estimate_days_remaining(&days)) {
        cJSON_AddNumberToObject(root, "days_remaining", days);
    } else {
        cJSON_AddNullToObject(root, "days_remaining");
    }

    return root;
}

bool battery_history_estimate_days_remaining(double *out_days)
{
    if (!storage_has_persistent_storage()) {
        return false;
    }
    FILE *f = fopen(BATTERY_HISTORY_PATH, "r");
    if (!f) {
        return false;
    }

    // Find the most recent battery-only (non-charging) run: any charging
    // entry breaks the run, and the next battery-only entry starts a fresh
    // one. Using only the final run keeps the estimate representative of
    // current behavior rather than being skewed by older, possibly
    // different usage patterns earlier in the log.
    time_t run_start_ts = 0, run_end_ts = 0;
    int run_start_pct = -1, run_end_pct = -1;
    bool in_run = false;

    char line[64];
    while (fgets(line, sizeof(line), f)) {
        long long ts = 0;
        int percent = 0, charging = 0;
        if (sscanf(line, "%lld,%d,%d", &ts, &percent, &charging) != 3) {
            continue;
        }
        if (charging) {
            in_run = false;
            continue;
        }
        if (!in_run) {
            run_start_ts = (time_t) ts;
            run_start_pct = percent;
            in_run = true;
        }
        run_end_ts = (time_t) ts;
        run_end_pct = percent;
    }
    fclose(f);

    if (!in_run || run_start_pct < 0 || run_end_pct < 0 || run_end_ts <= run_start_ts) {
        return false;  // Not enough battery-only history yet.
    }

    double elapsed_days = difftime(run_end_ts, run_start_ts) / 86400.0;
    double drop = (double) (run_start_pct - run_end_pct);
    if (elapsed_days <= 0 || drop <= 0) {
        return false;  // No measurable drain yet (e.g. only one reading so far).
    }

    double percent_per_day = drop / elapsed_days;
    if (run_end_pct <= BATTERY_HISTORY_TARGET_PERCENT) {
        *out_days = 0;
        return true;
    }
    *out_days = (run_end_pct - BATTERY_HISTORY_TARGET_PERCENT) / percent_per_day;
    return true;
}
