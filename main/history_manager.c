#include "history_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "esp_log.h"
#include "storage.h"

static const char *TAG = "history_manager";

static char **s_paths = NULL;
static int s_count = 0;
static int s_capacity = 0;

static bool grow_if_needed(void)
{
    if (s_count < s_capacity) {
        return true;
    }
    int new_capacity = (s_capacity == 0) ? 32 : s_capacity * 2;
    char **grown = realloc(s_paths, (size_t) new_capacity * sizeof(char *));
    if (!grown) {
        ESP_LOGE(TAG, "Failed to grow display-history array");
        return false;
    }
    s_paths = grown;
    s_capacity = new_capacity;
    return true;
}

esp_err_t history_manager_init(void)
{
    if (!storage_has_persistent_storage()) {
        ESP_LOGI(TAG, "No persistent storage - display history is RAM-only for this session");
        return ESP_OK;
    }

    FILE *f = fopen(DISPLAY_HISTORY_PATH, "r");
    if (!f) {
        return ESP_OK;  // No history file yet - fresh start.
    }

    char line[320];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0 || !grow_if_needed()) {
            continue;
        }
        s_paths[s_count] = strdup(line);
        if (s_paths[s_count]) {
            s_count++;
        }
    }
    fclose(f);
    ESP_LOGI(TAG, "Loaded %d display-history entries", s_count);
    return ESP_OK;
}

bool history_manager_has_shown(const char *image_path)
{
    if (!image_path) {
        return false;
    }
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_paths[i], image_path) == 0) {
            return true;
        }
    }
    return false;
}

void history_manager_mark_shown(const char *image_path)
{
    if (!image_path || history_manager_has_shown(image_path)) {
        return;
    }
    if (!grow_if_needed()) {
        return;
    }
    s_paths[s_count] = strdup(image_path);
    if (!s_paths[s_count]) {
        return;
    }
    s_count++;

    if (!storage_has_persistent_storage()) {
        return;
    }
    FILE *f = fopen(DISPLAY_HISTORY_PATH, "a");
    if (!f) {
        ESP_LOGW(TAG, "Failed to open display-history file for append");
        return;
    }
    fprintf(f, "%s\n", image_path);
    fclose(f);
}

void history_manager_clear(void)
{
    for (int i = 0; i < s_count; i++) {
        free(s_paths[i]);
    }
    s_count = 0;

    if (storage_has_persistent_storage()) {
        remove(DISPLAY_HISTORY_PATH);
    }
    ESP_LOGI(TAG, "Display history cleared");
}

int history_manager_count(void)
{
    return s_count;
}
