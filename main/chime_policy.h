#pragma once

#include <stdbool.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CHIME_PULL_MODE_DEFAULT "with_rotate"
#define CHIME_PLAY_WHEN_DEFAULT "after"

typedef enum {
    CHIME_PLAY_PREVIEW = 0,
    CHIME_PLAY_AFTER_DISPLAY = 1,
    CHIME_PLAY_BEFORE_DISPLAY = 2,
} chime_play_reason_t;

typedef enum {
    CHIME_PLAYED_NONE = 0,
    CHIME_PLAYED_PRESET,
    CHIME_PLAYED_WAV,
    CHIME_PLAYED_UPLOADED,
} chime_played_t;

// Display-path hook points. epaper_display() is blocking until the panel
// refresh finishes, so AFTER_REFRESH is after the e-ink draw completes.
typedef enum {
    CHIME_DISPLAY_HOOK_BEFORE_REFRESH = 0,  // after decode, before epaper_display
    CHIME_DISPLAY_HOOK_AFTER_REFRESH = 1,   // after epaper_display returns
} chime_display_hook_t;

static inline bool chime_play_when_is_valid(const char *when)
{
    return when && (strcmp(when, "after") == 0 || strcmp(when, "before") == 0);
}

// Unknown / empty values default to after (do not overlap a long e-ink draw).
static inline bool chime_play_when_is_before(const char *when)
{
    return when && strcmp(when, "before") == 0;
}

static inline bool chime_should_play_at_hook(const char *play_when, chime_display_hook_t hook)
{
    bool before = chime_play_when_is_before(play_when);
    if (hook == CHIME_DISPLAY_HOOK_BEFORE_REFRESH) {
        return before;
    }
    return !before;
}

static inline bool chime_reason_is_display_rotate(chime_play_reason_t reason)
{
    return reason == CHIME_PLAY_AFTER_DISPLAY || reason == CHIME_PLAY_BEFORE_DISPLAY;
}

// Preview fetches when the cache is empty or the caller asked to refresh.
// On a display rotate, with_rotate always re-GETs (before or after the panel
// wait, matching chime_play_when); once fetches only on first need.
static inline bool chime_should_fetch_url(chime_play_reason_t reason, bool with_rotate,
                                          bool have_cache, bool refresh)
{
    if (refresh || !have_cache) {
        return true;
    }
    return chime_reason_is_display_rotate(reason) && with_rotate;
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
