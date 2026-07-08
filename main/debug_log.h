#ifndef DEBUG_LOG_H
#define DEBUG_LOG_H

#include <stdbool.h>

#include "esp_err.h"

// Debug log capture: mirrors ESP_LOG console output to persistent storage.
// The log is kept at a bounded size via two-file rotation (current + previous
// generation), so enabling it long-term cannot fill the SD card.

// Start capture if it was enabled in config. Call once at boot, after
// storage_init() and config_manager_init().
esp_err_t debug_log_init(void);

// Enable/disable capture and persist the choice to NVS.
void debug_log_set_enabled(bool enabled);
bool debug_log_get_enabled(void);

// Drain buffered lines to storage and close the log file. Call before deep
// sleep / storage unmount. Capture resumes on the next boot.
void debug_log_flush(void);

// Paths of the rotated log generations (oldest first ordering is
// debug_log_old_path() then debug_log_current_path()).
const char *debug_log_current_path(void);
const char *debug_log_old_path(void);

// Serialize file access against the writer task and make sure everything
// buffered so far is on disk. Used by the HTTP download handler.
void debug_log_lock(void);
void debug_log_unlock(void);

#endif  // DEBUG_LOG_H
