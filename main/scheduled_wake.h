#ifndef SCHEDULED_WAKE_H
#define SCHEDULED_WAKE_H

#include <stdint.h>

#include "power_manager.h"

// Call once from app_main after shared initialization. Returns true when a
// timer/rotate wake has been handed off; the caller must return immediately.
// Allocation failure enters sleep without running the wake pipeline on main.
bool scheduled_wake_start(wakeup_source_t source);

// Wake pipeline implemented in main.c, called only by the dedicated task.
void deep_sleep_wake_main(wakeup_source_t source);

#endif
