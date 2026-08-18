// Host-test stub for freertos/task.h
#pragma once

#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

static inline void vTaskDelay(TickType_t ticks)
{
    (void) ticks;
}

#ifdef __cplusplus
}
#endif
