// Host-test stub for freertos/task.h
#pragma once

#include <stdint.h>

#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*TaskFunction_t)(void *);
typedef void *TaskHandle_t;

BaseType_t xTaskCreate(TaskFunction_t task, const char *name, uint32_t stack_bytes, void *arg,
                       UBaseType_t priority, TaskHandle_t *handle);

static inline void vTaskDelay(TickType_t ticks)
{
    (void) ticks;
}

#ifdef __cplusplus
}
#endif
