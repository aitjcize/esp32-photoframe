#ifndef WAKE_DIAGNOSTICS_H
#define WAKE_DIAGNOSTICS_H

#include <stdint.h>

#include "wake_diagnostics_record.h"

// RTC memory survives deep sleep and software resets. Call boot first, before
// board/storage initialization; logging can wait until debug capture starts.
void wake_diag_boot(void);
void wake_diag_log_previous(int reset_reason);
void wake_diag_set_source(uint32_t source);
void wake_diag_begin(wake_phase_t phase);
void wake_diag_complete(wake_phase_t phase);
void wake_diag_wifi_disconnect(uint16_t reason);

#endif
