#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "wake_diagnostics_record.h"

int main(void)
{
    wake_diag_record_t record;
    memset(&record, 0xa5, sizeof(record));
    assert(!wake_diag_record_valid(&record));

    wake_diag_record_init(&record, 7);
    assert(sizeof(record) <= 64);
    assert(wake_diag_record_valid(&record));
    assert(record.sequence == 7);
    assert(record.generation == 1);
    assert(record.completed_phase == WAKE_PHASE_NONE);
    assert(record.active_phase == WAKE_PHASE_BOOT);

    record.wake_source = 2;
    record.completed_phase = WAKE_PHASE_WIFI;
    record.active_phase = WAKE_PHASE_ROTATION;
    record.min_stack_bytes = 816;
    record.main_stack_bytes = 816;
    record.wifi_disconnect_reason = 201;
    memcpy(record.task_name, "main", 5);
    wake_diag_record_seal(&record);

    // Round-trip the exact bytes that survive a reset.
    uint8_t image[sizeof(record)];
    memcpy(image, &record, sizeof(image));
    wake_diag_record_t restored;
    memcpy(&restored, image, sizeof(restored));
    assert(wake_diag_record_valid(&restored));
    assert(restored.completed_phase == WAKE_PHASE_WIFI);
    assert(restored.active_phase == WAKE_PHASE_ROTATION);
    assert(restored.min_stack_bytes == 816);
    assert(restored.main_stack_bytes == 816);
    assert(restored.wifi_disconnect_reason == 201);
    assert(strcmp(restored.task_name, "main") == 0);

    wake_diag_record_t slots[2] = {record, restored};
    slots[1].generation = 2;
    wake_diag_record_seal(&slots[1]);
    assert(wake_diag_record_latest(slots) == 1);
    slots[1].checksum ^= 1;  // reset during an RTC update
    assert(wake_diag_record_latest(slots) == 0);
    slots[0].checksum ^= 1;
    assert(wake_diag_record_latest(slots) == -1);
    slots[0] = record;
    slots[0].generation = UINT32_MAX;
    wake_diag_record_seal(&slots[0]);
    slots[1] = record;
    slots[1].generation = 0;
    wake_diag_record_seal(&slots[1]);
    assert(wake_diag_record_latest(slots) == 1);

    restored.version++;
    assert(!wake_diag_record_valid(&restored));
    restored = record;
    restored.size--;
    assert(!wake_diag_record_valid(&restored));
    restored = record;
    restored.active_phase = WAKE_PHASE_SLEEP + 1;
    wake_diag_record_seal(&restored);
    assert(!wake_diag_record_valid(&restored));
    restored = record;
    ((uint8_t *) &restored)[offsetof(wake_diag_record_t, wifi_disconnect_reason)] ^= 1;
    assert(!wake_diag_record_valid(&restored));

    assert(strcmp(wake_diag_phase_name(WAKE_PHASE_SLEEP), "sleep") == 0);
    assert(strcmp(wake_diag_phase_name((wake_phase_t) 255), "invalid") == 0);
    puts("wake diagnostics record tests passed");
    return 0;
}
