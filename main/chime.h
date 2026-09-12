#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "chime_name.h"
#include "chime_policy.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CHIME_MAX_STORED 16

typedef struct {
    char name[CHIME_FILENAME_MAX_LEN];
    size_t size;
} chime_entry_t;

typedef struct {
    chime_played_t played;
    bool fetched;
    esp_err_t fetch_err;
} chime_play_result_t;

// Play the currently selected chime (preset, last cached URL WAV, or an
// uploaded file). Preview fetches when the cache is empty or refresh is set.
// On a display rotate (before or after the panel wait), with_rotate also
// GET-replaces the URL cache before playing. Falls back to the selected
// preset if WAV fetch/play fails. Caller should check chime_enabled.
esp_err_t chime_play(chime_play_reason_t reason);
esp_err_t chime_play_detailed(chime_play_reason_t reason, bool refresh, chime_play_result_t *out);

// GET chime_url, validate PCM WAV, and replace the on-device cache. Does not play.
esp_err_t chime_pull_url(void);

bool chime_cache_exists(void);
size_t chime_cache_size(void);
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
