#pragma once

#include <stdbool.h>

#include "crash_record.h"
#include "esp_err.h"

// If the previous boot left a core dump, summarise it into the last-crash
// record in NVS, log it, and erase the dump. Call once at boot, after NVS and
// debug logging are up.
void crash_log_capture(void);

// Read the last-crash record; false when there is none.
bool crash_log_load(crash_record_t *rec);

esp_err_t crash_log_clear(void);
