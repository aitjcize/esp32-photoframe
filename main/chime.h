#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CHIME_PLAY_PREVIEW = 0,
    CHIME_PLAY_AFTER_DISPLAY = 1,
} chime_play_reason_t;

// Play the currently selected chime (preset or last cached WAV).
// Preview uses the cache as-is (fetches only on first need). After display,
// with_rotate also GET-replaces the cache before playing. Falls back to the
// selected preset if WAV fetch/play fails. Caller should check chime_enabled.
esp_err_t chime_play(chime_play_reason_t reason);

bool chime_cache_exists(void);
void chime_invalidate_cache(void);

bool chime_preset_is_valid(const char *preset);
bool chime_source_is_valid(const char *source);
bool chime_pull_mode_is_valid(const char *mode);

#ifdef __cplusplus
}
#endif
