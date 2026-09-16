#include "wake_diagnostics.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "wake_diag";
RTC_NOINIT_ATTR static wake_diag_record_t s_rtc_records[2];
static int s_current;
static wake_diag_record_t s_previous;
static bool s_previous_valid;
static portMUX_TYPE s_record_lock = portMUX_INITIALIZER_UNLOCKED;

static void sample_stack(wake_diag_record_t *record, uint32_t bytes, const char *name)
{
    if (name && strcmp(name, "main") == 0 &&
        (record->main_stack_bytes == 0 || bytes < record->main_stack_bytes)) {
        record->main_stack_bytes = bytes;
    }
    if (record->min_stack_bytes == 0 || bytes < record->min_stack_bytes) {
        record->min_stack_bytes = bytes;
        if (name) {
            strncpy(record->task_name, name, sizeof(record->task_name) - 1);
            record->task_name[sizeof(record->task_name) - 1] = '\0';
        }
    }
}

// Always write the inactive slot. A reset during the copy leaves the current
// slot's checksum intact, so boot can still report the last complete marker.
static void commit_locked(wake_diag_record_t *next)
{
    next->generation = s_rtc_records[s_current].generation + 1;
    wake_diag_record_seal(next);
    int destination = 1 - s_current;
    s_rtc_records[destination] = *next;
    s_current = destination;
}

void wake_diag_boot(void)
{
    int previous_slot = wake_diag_record_latest(s_rtc_records);
    s_previous_valid = previous_slot >= 0;
    if (s_previous_valid) {
        s_previous = s_rtc_records[previous_slot];
    }
    wake_diag_record_t next;
    wake_diag_record_init(&next, s_previous_valid ? s_previous.sequence + 1 : 1);
    uint32_t bytes = uxTaskGetStackHighWaterMark(NULL);
    const char *name = pcTaskGetName(NULL);
    portENTER_CRITICAL(&s_record_lock);
    if (!s_previous_valid) {
        // There is no valid old slot to preserve on the first boot.
        s_current = 0;
        s_rtc_records[0] = next;
    } else {
        s_current = previous_slot;
    }
    sample_stack(&next, bytes, name);
    commit_locked(&next);
    portEXIT_CRITICAL(&s_record_lock);
}

void wake_diag_log_previous(int reset_reason)
{
    if (!s_previous_valid) {
        ESP_LOGI(TAG,
                 "Previous wake record unavailable (first boot, power loss, or corrupt record)");
        return;
    }
    ESP_LOGI(
        TAG,
        "Previous wake #%lu: reset=%d source=%lu completed=%s active=%s "
        "stack_min=%lu bytes task=%s main_stack_min=%lu bytes wifi_disconnect=%u",
        (unsigned long) s_previous.sequence, reset_reason, (unsigned long) s_previous.wake_source,
        wake_diag_phase_name((wake_phase_t) s_previous.completed_phase),
        wake_diag_phase_name((wake_phase_t) s_previous.active_phase),
        (unsigned long) s_previous.min_stack_bytes,
        s_previous.task_name[0] ? s_previous.task_name : "unknown",
        (unsigned long) s_previous.main_stack_bytes, (unsigned) s_previous.wifi_disconnect_reason);
}

void wake_diag_set_source(uint32_t source)
{
    portENTER_CRITICAL(&s_record_lock);
    wake_diag_record_t next = s_rtc_records[s_current];
    next.wake_source = source;
    commit_locked(&next);
    portEXIT_CRITICAL(&s_record_lock);
}

void wake_diag_begin(wake_phase_t phase)
{
    uint32_t bytes = uxTaskGetStackHighWaterMark(NULL);
    const char *name = pcTaskGetName(NULL);
    portENTER_CRITICAL(&s_record_lock);
    wake_diag_record_t next = s_rtc_records[s_current];
    next.active_phase = phase;
    sample_stack(&next, bytes, name);
    commit_locked(&next);
    portEXIT_CRITICAL(&s_record_lock);
}

void wake_diag_complete(wake_phase_t phase)
{
    uint32_t bytes = uxTaskGetStackHighWaterMark(NULL);
    const char *name = pcTaskGetName(NULL);
    portENTER_CRITICAL(&s_record_lock);
    wake_diag_record_t next = s_rtc_records[s_current];
    next.completed_phase = phase;
    next.active_phase = WAKE_PHASE_NONE;
    sample_stack(&next, bytes, name);
    commit_locked(&next);
    portEXIT_CRITICAL(&s_record_lock);
}

void wake_diag_wifi_disconnect(uint16_t reason)
{
    portENTER_CRITICAL(&s_record_lock);
    wake_diag_record_t next = s_rtc_records[s_current];
    next.wifi_disconnect_reason = reason;
    commit_locked(&next);
    portEXIT_CRITICAL(&s_record_lock);
}
