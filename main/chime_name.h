#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CHIME_FILENAME_MAX_LEN 64

bool chime_filename_is_valid(const char *name);

// Turn an upload filename (possibly a path) into a safe "name.wav".
// Returns false if nothing usable remains.
bool chime_sanitize_filename(const char *original, char *dest, size_t dest_len);

#ifdef __cplusplus
}
#endif
