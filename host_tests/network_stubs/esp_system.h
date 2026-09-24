#pragma once
#include "esp_err.h"
typedef enum { ESP_RST_POWERON, ESP_RST_SW, ESP_RST_DEEPSLEEP, ESP_RST_PANIC } esp_reset_reason_t;
esp_reset_reason_t esp_reset_reason(void);
void esp_restart(void);
#define ESP_ERROR_CHECK(x) \
    do {                   \
        if ((x) != ESP_OK) \
            abort();       \
    } while (0)
