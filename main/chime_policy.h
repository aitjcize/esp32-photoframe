#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CHIME_PLAY_PREVIEW = 0,
    CHIME_PLAY_AFTER_DISPLAY = 1,
} chime_play_reason_t;

typedef enum {
    CHIME_PLAYED_NONE = 0,
    CHIME_PLAYED_PRESET,
    CHIME_PLAYED_WAV,
    CHIME_PLAYED_UPLOADED,
} chime_played_t;

// Preview fetches when the cache is empty or the caller asked to refresh.
// After display, with_rotate always re-GETs; once fetches only on first need.
static inline bool chime_should_fetch_url(chime_play_reason_t reason, bool with_rotate,
                                          bool have_cache, bool refresh)
{
    if (refresh || !have_cache) {
        return true;
    }
    return reason == CHIME_PLAY_AFTER_DISPLAY && with_rotate;
}

static inline const char *chime_played_name(chime_played_t played)
{
    switch (played) {
    case CHIME_PLAYED_WAV:
        return "wav";
    case CHIME_PLAYED_UPLOADED:
        return "uploaded";
    case CHIME_PLAYED_PRESET:
        return "preset";
    case CHIME_PLAYED_NONE:
    default:
        return "none";
    }
}

#ifdef __cplusplus
}
#endif
