#pragma once
#include "FreeRTOS.h"
void vTaskDelay(TickType_t ticks);
BaseType_t xTaskCreate(void (*task)(void *), const char *name, uint32_t stack, void *arg,
                       unsigned priority, void *handle);
