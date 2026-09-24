#pragma once
#include <stdint.h>
#include <stdlib.h>
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) ((void) (lock))
#define portEXIT_CRITICAL(lock) ((void) (lock))
#define pdMS_TO_TICKS(ms) ((TickType_t) (ms))
#define configTICK_RATE_HZ 1000
#define portMAX_DELAY UINT32_MAX
#define pdFALSE 0
#define pdTRUE 1
#define pdPASS 1
#define BIT0 1
#define BIT1 2
