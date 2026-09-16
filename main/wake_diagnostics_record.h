#ifndef WAKE_DIAGNOSTICS_RECORD_H
#define WAKE_DIAGNOSTICS_RECORD_H

#include <stdbool.h>
#include <stdint.h>

#define WAKE_DIAG_MAGIC 0x57414b45u
#define WAKE_DIAG_VERSION 2u

typedef enum {
    WAKE_PHASE_NONE,
    WAKE_PHASE_BOOT,
    WAKE_PHASE_BOARD,
    WAKE_PHASE_STORAGE,
    WAKE_PHASE_CONFIG,
    WAKE_PHASE_RTC,
    WAKE_PHASE_SERVICES,
    WAKE_PHASE_WIFI,
    WAKE_PHASE_PERIODIC,
    WAKE_PHASE_HA_CHECK,
    WAKE_PHASE_ROTATION,
    WAKE_PHASE_HA_UPDATE,
    WAKE_PHASE_HTTP_WINDOW,
    WAKE_PHASE_SLEEP,
} wake_phase_t;

// Fixed-size RTC record. A checksum rejects uninitialized or torn data.
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t sequence;
    uint32_t generation;
    uint32_t wake_source;
    uint32_t completed_phase;
    uint32_t active_phase;
    uint32_t min_stack_bytes;
    uint32_t main_stack_bytes;
    uint16_t wifi_disconnect_reason;
    uint16_t reserved;
    char task_name[16];
    uint32_t checksum;
} wake_diag_record_t;

void wake_diag_record_init(wake_diag_record_t *record, uint32_t sequence);
void wake_diag_record_seal(wake_diag_record_t *record);
bool wake_diag_record_valid(const wake_diag_record_t *record);
int wake_diag_record_latest(const wake_diag_record_t records[2]);
const char *wake_diag_phase_name(wake_phase_t phase);

#endif
