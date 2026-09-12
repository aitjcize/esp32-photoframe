#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "chime_name.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CHIME_MAX_STORED 16

typedef enum {
    CHIME_PLAY_PREVIEW = 0,
    CHIME_PLAY_AFTER_DISPLAY = 1,
} chime_play_reason_t;

typedef struct {
    char name[CHIME_FILENAME_MAX_LEN];
    size_t size;
} chime_entry_t;

// Play the currently selected chime (preset, last cached URL WAV, or an
// uploaded file). Preview uses the cache as-is (fetches only on first need).
// After display, with_rotate also GET-replaces the URL cache before playing.
// Falls back to the selected preset if WAV fetch/play fails. Caller should
// check chime_enabled.
esp_err_t chime_play(chime_play_reason_t reason);

bool chime_cache_exists(void);
void chime_invalidate_cache(void);

bool chime_preset_is_valid(const char *preset);
bool chime_source_is_valid(const char *source);
bool chime_pull_mode_is_valid(const char *mode);

esp_err_t chime_ensure_dir(void);
esp_err_t chime_validate_wav_file(const char *path);
esp_err_t chime_install_upload(const char *tmp_path, const char *original_filename, char *out_name,
                               size_t out_len);
int chime_list(chime_entry_t *out, int max);
esp_err_t chime_delete(const char *filename);
bool chime_uploaded_exists(const char *filename);

#ifdef __cplusplus
}
#endif
