// Host-test stub for esp_task_wdt.h
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

static inline esp_err_t esp_task_wdt_reset(void)
{
    return ESP_OK;
}

#ifdef __cplusplus
}
#endif
