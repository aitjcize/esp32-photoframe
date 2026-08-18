// Host-test stub for freertos/FreeRTOS.h
#pragma once

typedef int TickType_t;
typedef int BaseType_t;

#define pdMS_TO_TICKS(ms) (ms)
#define portTICK_PERIOD_MS 1
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
