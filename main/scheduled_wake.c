#include "scheduled_wake.h"

#include <stdint.h>
#include <stdlib.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "scheduled_wake";

// ESP-IDF task stack sizes are in bytes. Validate this initial budget against
// the sleep-entry high-water mark on each board and rotation mode.
#define SCHEDULED_WAKE_STACK_BYTES 12288

static void sleep_after_failure(void)
{
    power_manager_enter_sleep();
    // Deep sleep must not return. Panic rather than leave an unattended frame
    // awake or let a FreeRTOS task return to its caller.
    abort();
}

static void scheduled_wake_task(void *arg)
{
    deep_sleep_wake_main((wakeup_source_t) (intptr_t) arg);
    ESP_LOGE(TAG, "Wake pipeline returned unexpectedly; entering sleep");
    sleep_after_failure();
}

bool scheduled_wake_start(wakeup_source_t source)
{
    if (source != WAKEUP_SOURCE_TIMER && source != WAKEUP_SOURCE_ROTATE_BUTTON) {
        return false;
    }

    ESP_LOGI(TAG, "Starting dedicated wake task (%d bytes)", SCHEDULED_WAKE_STACK_BYTES);
    // Pass the source by value: app_main's stack is reclaimed when it returns.
    if (xTaskCreate(scheduled_wake_task, "deep_sleep_wake", SCHEDULED_WAKE_STACK_BYTES,
                    (void *) (intptr_t) source, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Could not allocate wake task; skipping rotation and entering sleep");
        sleep_after_failure();
    }
    return true;
}
