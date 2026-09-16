#include "wake_diagnostics_record.h"

#include <stddef.h>
#include <string.h>

static uint32_t record_checksum(const wake_diag_record_t *record)
{
    const uint8_t *bytes = (const uint8_t *) record;
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < offsetof(wake_diag_record_t, checksum); ++i) {
        hash = (hash ^ bytes[i]) * 16777619u;
    }
    return hash;
}

void wake_diag_record_init(wake_diag_record_t *record, uint32_t sequence)
{
    memset(record, 0, sizeof(*record));
    record->magic = WAKE_DIAG_MAGIC;
    record->version = WAKE_DIAG_VERSION;
    record->size = sizeof(*record);
    record->sequence = sequence;
    record->generation = 1;
    record->active_phase = WAKE_PHASE_BOOT;
    wake_diag_record_seal(record);
}

void wake_diag_record_seal(wake_diag_record_t *record)
{
    record->checksum = record_checksum(record);
}

bool wake_diag_record_valid(const wake_diag_record_t *record)
{
    return record->magic == WAKE_DIAG_MAGIC && record->version == WAKE_DIAG_VERSION &&
           record->size == sizeof(*record) && record->completed_phase <= WAKE_PHASE_SLEEP &&
           record->active_phase <= WAKE_PHASE_SLEEP && record->checksum == record_checksum(record);
}

int wake_diag_record_latest(const wake_diag_record_t records[2])
{
    bool first = wake_diag_record_valid(&records[0]);
    bool second = wake_diag_record_valid(&records[1]);
    if (!first && !second) {
        return -1;
    }
    if (!first) {
        return 1;
    }
    if (!second) {
        return 0;
    }
    // Modular comparison also handles a 32-bit generation wrap.
    uint32_t difference = records[1].generation - records[0].generation;
    return difference != 0 && difference < 0x80000000u ? 1 : 0;
}

const char *wake_diag_phase_name(wake_phase_t phase)
{
    static const char *const names[] = {
        "none", "boot",     "board",    "storage",  "config",    "rtc",         "services",
        "wifi", "periodic", "ha_check", "rotation", "ha_update", "http_window", "sleep",
    };
    return phase <= WAKE_PHASE_SLEEP ? names[phase] : "invalid";
}
