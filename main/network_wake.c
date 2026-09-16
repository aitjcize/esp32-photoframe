#include "network_wake.h"

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "network_policy.h"

#define RETAINED_MAGIC 0x4e574b31U
// NOINIT also survives esp_restart; ordinary RTC_DATA_ATTR is reinitialized
// on a software reset. Validate before use and discard on unrelated resets.
RTC_NOINIT_ATTR static volatile struct {
    uint32_t magic;
    uint8_t failures;
    uint32_t recovery_magic;
} retained;

static bool active;
static bool recovering;
static int64_t deadline_us;

bool network_wake_init(void)
{
    active = false;
    deadline_us = 0;
    esp_reset_reason_t reason = esp_reset_reason();
    bool valid = retained.magic == RETAINED_MAGIC && retained.failures <= NETWORK_FAILURE_LIMIT;
    bool recovery = valid && retained.recovery_magic == RETAINED_MAGIC && reason == ESP_RST_SW;
    if (!valid || (reason != ESP_RST_DEEPSLEEP && !recovery)) {
        retained.magic = RETAINED_MAGIC;
        retained.failures = 0;
    }
    retained.recovery_magic = 0;
    recovering = recovery;
    if (recovery && retained.failures == 0) {
        retained.failures = 1;
    }
    return recovery;
}

static void deadline_task(void *arg)
{
    // Separate from esp_timer's task: periodic callbacks can themselves block
    // on network work. Never tear down storage/panel from under another task.
    for (;;) {
        int remaining =
            network_deadline_remaining_ms(deadline_us, esp_timer_get_time(), NETWORK_WAKE_LIMIT_MS);
        if (remaining == 0) {
            retained.recovery_magic = RETAINED_MAGIC;
            esp_restart();
        }
        vTaskDelay(pdMS_TO_TICKS(remaining) + 1);
    }
}

esp_err_t network_wake_begin(void)
{
    if (active) {
        return ESP_ERR_INVALID_STATE;
    }
    // Record a failure pessimistically; only a completed operation clears it.
    // RTC-only writes avoid flash wear on repeated offline wakes.
    retained.failures = network_failure_increment(retained.failures);
    deadline_us = esp_timer_get_time() + NETWORK_WAKE_LIMIT_MS * 1000LL;
    active = true;
    if (xTaskCreate(deadline_task, "wake_deadline", 3072, NULL, 6, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;  // Caller must sleep without starting networking.
    }
    ESP_LOGI("network_wake", "Wake deadline: %d ms; failure backoff: %lu s", NETWORK_WAKE_LIMIT_MS,
             (unsigned long) network_wake_backoff_seconds());
    return ESP_OK;
}

bool network_wake_active(void)
{
    return active;
}

int network_wake_timeout_ms(int maximum_ms)
{
    return active ? network_deadline_remaining_ms(deadline_us - NETWORK_WAKE_FINISH_MS * 1000LL,
                                                  esp_timer_get_time(), maximum_ms)
                  : maximum_ms;
}

int64_t network_wake_deadline_us(int maximum_ms)
{
    return esp_timer_get_time() + network_wake_timeout_ms(maximum_ms) * 1000LL;
}

void network_wake_succeeded(void)
{
    if (active) {
        retained.failures = 0;
    }
}

uint32_t network_wake_backoff_seconds(void)
{
    // An early timer re-sleep must not restart the backoff interval.
    return (active || recovering) ? network_backoff_seconds(retained.failures) : 0;
}
