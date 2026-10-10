#ifndef CONFIG_TRACK_H
#define CONFIG_TRACK_H

#include <stdbool.h>

#include "cJSON.h"

/**
 * @file config_track.h
 * @brief Which fields of a config request apply_config_from_json() ignored.
 *
 * The handlers of apply_config_from_json() each look one field up with cJSON_GetObjectItem() and
 * take it only when its JSON type is the one they read (`item && cJSON_IsString(item)`). A field of
 * another type - `"rotation_interval": "soon"` - is skipped without a word, and so is a key no
 * handler knows (a typo, or a setting of another firmware): the request still answers "success".
 *
 * This remembers, for the direct children of one request object, which were looked up and which
 * were taken, so the answer can name the rest. It is a small state machine of one request at a time
 * (a second begin while one is running tracks nothing); the object being applied must not be
 * changed or freed between begin and end.
 */

// More fields than this in one request are applied as always, the extra ones are not reported.
#define CONFIG_TRACK_MAX_KEYS 512

// Starts tracking the children of `root`. False (and nothing is tracked) when `root` is not an
// object or another request is being tracked.
bool config_track_begin(const cJSON *root);

// cJSON_GetObjectItem() that also remembers a key of the tracked object as looked up.
cJSON *config_track_get(const cJSON *object, const char *key);

// Passes `matched` through; a true marks `item` (a field of the tracked object) as taken. Items of
// anywhere else are left alone, so it can sit in front of every type check of the handlers.
bool config_track_taken(const cJSON *item, bool matched);

// Stops tracking. Returns {"ignored": [keys], "unknown": [keys]} - "ignored": a key a handler
// looked up but whose JSON type it does not read, "unknown": a key no handler looked up; an empty
// list is left out and JSON null never counts - or NULL when there is nothing to report (or nothing
// was tracked). The caller frees it.
cJSON *config_track_end(void);

#endif
