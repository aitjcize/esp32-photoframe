#ifndef HISTORY_MANAGER_H
#define HISTORY_MANAGER_H

#include <stdbool.h>

#include "esp_err.h"

// Tracks which images have already been shown (identified by their full
// storage path) so random rotation - and the Telegram-mode fallback
// rotation - can cycle through every image once before repeating. Persisted
// to a plain-text file on persistent storage (SD card or internal flash),
// one path per line, so the history survives a reboot; falls back to
// RAM-only tracking for the current session if no persistent storage is
// mounted.

// Loads the history file (if any) into RAM. Call once at boot, after
// storage_init(). Always returns ESP_OK - a missing/unreadable file just
// means an empty history.
esp_err_t history_manager_init(void);

// True if this exact path has already been marked shown since the last
// clear.
bool history_manager_has_shown(const char *image_path);

// Marks a path as shown. No-op if it's already marked (keeps the history
// file from growing on repeated calls for the same path, e.g. a fixed
// "current image" temp file).
void history_manager_mark_shown(const char *image_path);

// Clears the whole history (RAM and the persisted file), starting a fresh
// cycle.
void history_manager_clear(void);

// Number of entries currently tracked.
int history_manager_count(void);

#endif
