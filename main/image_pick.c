#include "image_pick.h"

#include <string.h>

static bool same_path(const char *a, const char *b)
{
    return a && b && strcmp(a, b) == 0;
}

static void set_path(image_pick_t *pick, const char *path)
{
    strncpy(pick->path, path, sizeof(pick->path) - 1);
    pick->path[sizeof(pick->path) - 1] = '\0';
}

void image_pick_init(image_pick_t *pick, const char *avoid, const char *exclude)
{
    memset(pick, 0, sizeof(*pick));
    pick->avoid = (avoid && avoid[0]) ? avoid : NULL;
    pick->exclude = (exclude && exclude[0]) ? exclude : NULL;
}

void image_pick_offer(image_pick_t *pick, const char *path, uint32_t random)
{
    if (same_path(path, pick->exclude)) {
        return;
    }
    if (same_path(path, pick->avoid)) {
        pick->avoid_seen = true;
        return;
    }
    // The n-th candidate replaces the pick with probability 1/n, which leaves
    // every candidate equally likely once the walk is done
    pick->candidates++;
    if (random % pick->candidates == 0) {
        set_path(pick, path);
    }
}

const char *image_pick_finish(image_pick_t *pick)
{
    if (pick->candidates > 0) {
        return pick->path;
    }
    if (pick->avoid_seen) {
        set_path(pick, pick->avoid);
        return pick->path;
    }
    return NULL;
}
