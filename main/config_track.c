#include "config_track.h"

#include <stdint.h>

#define STATE_LOOKED 0x01
#define STATE_TAKEN 0x02

static int s_busy;  // 1 while a request is tracked (claimed with an atomic exchange)
static const cJSON *s_root;
static uint8_t s_state[CONFIG_TRACK_MAX_KEYS];

// The position of `item` among the children of the tracked object, -1 when it is none of them (or
// past the limit).
static int index_of(const cJSON *item)
{
    int i = 0;
    for (const cJSON *c = s_root->child; c && i < CONFIG_TRACK_MAX_KEYS; c = c->next, i++) {
        if (c == item) {
            return i;
        }
    }
    return -1;
}

bool config_track_begin(const cJSON *root)
{
    if (!root || !cJSON_IsObject(root)) {
        return false;
    }
    if (__atomic_exchange_n(&s_busy, 1, __ATOMIC_ACQUIRE)) {
        return false;  // another request is being tracked
    }
    s_root = root;
    for (int i = 0; i < CONFIG_TRACK_MAX_KEYS; i++) {
        s_state[i] = 0;
    }
    return true;
}

cJSON *config_track_get(const cJSON *object, const char *key)
{
    cJSON *item = cJSON_GetObjectItem(object, key);
    if (item && s_root && object == s_root) {
        int i = index_of(item);
        if (i >= 0) {
            s_state[i] |= STATE_LOOKED;
        }
    }
    return item;
}

bool config_track_taken(const cJSON *item, bool matched)
{
    if (matched && item && s_root) {
        int i = index_of(item);
        if (i >= 0) {
            s_state[i] |= STATE_TAKEN;
        }
    }
    return matched;
}

cJSON *config_track_end(void)
{
    if (!s_root) {
        return NULL;
    }
    cJSON *ignored = cJSON_CreateArray();
    cJSON *unknown = cJSON_CreateArray();
    int i = 0;
    for (const cJSON *c = s_root->child; c && i < CONFIG_TRACK_MAX_KEYS; c = c->next, i++) {
        if (!c->string || cJSON_IsNull(c)) {
            continue;  // JSON null is "no value" (docs/API.md: it leaves a setting unchanged), not
                       // a wrong type
        }
        if (!(s_state[i] & STATE_LOOKED)) {
            if (unknown) {
                cJSON_AddItemToArray(unknown, cJSON_CreateString(c->string));
            }
        } else if (!(s_state[i] & STATE_TAKEN)) {
            if (ignored) {
                cJSON_AddItemToArray(ignored, cJSON_CreateString(c->string));
            }
        }
    }
    s_root = NULL;
    __atomic_store_n(&s_busy, 0, __ATOMIC_RELEASE);

    cJSON *report = NULL;
    if (ignored && cJSON_GetArraySize(ignored) > 0) {
        report = cJSON_CreateObject();
        if (report) {
            cJSON_AddItemToObject(report, "ignored", ignored);
            ignored = NULL;
        }
    }
    if (unknown && cJSON_GetArraySize(unknown) > 0) {
        if (!report) {
            report = cJSON_CreateObject();
        }
        if (report) {
            cJSON_AddItemToObject(report, "unknown", unknown);
            unknown = NULL;
        }
    }
    cJSON_Delete(ignored);  // NULL, or an empty list
    cJSON_Delete(unknown);
    return report;
}
