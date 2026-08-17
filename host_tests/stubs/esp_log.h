// Host-test stub for esp_log.h — errors/warnings go to stderr, the rest is
// suppressed to keep test output readable.
#pragma once

#include <stdio.h>

#define ESP_LOGE(tag, fmt, ...) fprintf(stderr, "E (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) fprintf(stderr, "W (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) \
    do {                        \
    } while (0)
#define ESP_LOGD(tag, fmt, ...) \
    do {                        \
    } while (0)
#define ESP_LOGV(tag, fmt, ...) \
    do {                        \
    } while (0)
