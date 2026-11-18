#pragma once

#include <stdbool.h>
#include <stdint.h>

// Picks one image uniformly at random while the caller walks the albums
// (reservoir sampling), so a random rotation needs one path buffer however
// many images there are, instead of a list of every path (#165).
//
// Each image is offered once with a fresh random number. `avoid` (the image on
// the panel now) is picked only when nothing else is offered; `exclude` (an
// image that just failed to display) is never picked. Either may be NULL.
//
// Kept free of ESP-IDF headers so the selection is host-tested.

#define IMAGE_PICK_PATH_MAX 512

typedef struct {
    const char *avoid;
    const char *exclude;
    uint32_t candidates;  // images offered that may be picked
    bool avoid_seen;
    char path[IMAGE_PICK_PATH_MAX];
} image_pick_t;

void image_pick_init(image_pick_t *pick, const char *avoid, const char *exclude);

// Offer the next image; `random` is any uniformly distributed 32-bit value
void image_pick_offer(image_pick_t *pick, const char *path, uint32_t random);

// The picked path (owned by `pick`, valid until it is re-initialized), or NULL
// when no image was offered
const char *image_pick_finish(image_pick_t *pick);
