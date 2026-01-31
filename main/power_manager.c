#include "power_manager.h"

#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_log.h>
#include <esp_pm.h>
#include <esp_sleep.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <time.h>

#include "board_hal.h"
#include "config.h"
#include "config_manager.h"
#include "ha_integration.h"
#include "periodic_tasks.h"
#include "utils.h"

// RTC memory to store expected wakeup time (persists across deep sleep)
RTC_DATA_ATTR static time_t expected_wakeup_time = 0;

static const char *TAG = "power_manager";

static TaskHandle_t sleep_timer_task_handle = NULL;
static TaskHandle_t rotation_timer_task_handle = NULL;
static int64_t next_sleep_time = 0;     // Use absolute time for sleep timer
static bool deep_sleep_enabled = true;  // Enabled by default, can be disabled for HA integration
static esp_sleep_wakeup_cause_t last_wakeup_cause = ESP_SLEEP_WAKEUP_UNDEFINED;
static int64_t next_rotation_time = 0;  // Use absolute time for rotation
static uint64_t ext1_wakeup_pin_mask = 0;

static int get_next_rotation_interval(void)
{
    int rotate_interval = config_manager_get_rotate_interval();
    if (config_manager_get_auto_rotate_aligned()) {
        return calculate_next_aligned_wakeup(rotate_interval);
    }
    return rotate_interval;
}

static void rotation_timer_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        // Run active rotation when:
        // 1. USB is connected (device stays awake), OR
        // 2. Deep sleep is disabled (device stays awake on battery)
        bool should_use_active_rotation = board_hal_is_usb_connected() || !deep_sleep_enabled;

        if (!should_use_active_rotation) {
            // Device will auto-sleep after 120 seconds, no need to reset timer
            continue;
        }

        // Handle active rotation when device stays awake and auto-rotate enabled
        if (config_manager_get_auto_rotate()) {
            // Check if we're in sleep schedule
            if (config_manager_is_in_sleep_schedule()) {
                // During sleep schedule, don't rotate
                next_rotation_time = 0;  // Reset timer
                continue;
            }

            int64_t now = esp_timer_get_time();  // Get absolute time in microseconds

            if (next_rotation_time == 0) {
                // Initialize next rotation time
                int seconds_until_next = get_next_rotation_interval();

                next_rotation_time = now + (seconds_until_next * 1000000LL);
                const char *reason =
                    board_hal_is_usb_connected() ? "USB powered" : "deep sleep disabled";
                ESP_LOGI(TAG, "Active rotation scheduled in %d seconds (%s, %s)",
                         seconds_until_next,
                         config_manager_get_auto_rotate_aligned() ? "clock-aligned" : "interval",
                         reason);
            } else if (now >= next_rotation_time) {
                // Time to rotate
                const char *reason =
                    board_hal_is_usb_connected() ? "USB powered" : "deep sleep disabled";
                ESP_LOGI(TAG, "Active rotation triggered (%s)", reason);

                trigger_image_rotation();
                ha_notify_update();

                // Schedule next rotation
                int seconds_until_next = get_next_rotation_interval();

                next_rotation_time = now + (seconds_until_next * 1000000LL);
                ESP_LOGI(TAG, "Next rotation scheduled in %d seconds (%s)", seconds_until_next,
                         config_manager_get_auto_rotate_aligned() ? "clock-aligned" : "interval");
            }
        } else {
            next_rotation_time = 0;  // Reset if auto-rotate disabled
        }
    }
}

static void sleep_timer_task(void *arg)
{
    int64_t last_blink_time = 0;
    int64_t last_log_time = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

#ifndef DEBUG_DEEP_SLEEP_WAKE
        // Skip auto-sleep when USB is connected
        if (board_hal_is_usb_connected()) {
            // Reset timer so it doesn't trigger immediately when USB is unplugged
            next_sleep_time = 0;
            continue;
        }
#endif

        // Handle auto-sleep timer when on battery (only if deep sleep is enabled)
        if (deep_sleep_enabled) {
            int64_t now = esp_timer_get_time();

            if (next_sleep_time == 0) {
                // Initialize sleep timer
                next_sleep_time = now + (AUTO_SLEEP_TIMEOUT_SEC * 1000000LL);
                last_blink_time = now;
                last_log_time = now;
                ESP_LOGI(TAG, "Auto-sleep timer started, will sleep in %d seconds",
                         AUTO_SLEEP_TIMEOUT_SEC);
            }

            int64_t remaining_us = next_sleep_time - now;
            int32_t remaining_sec = (int32_t) (remaining_us / 1000000LL);

            if (remaining_sec > 0) {
                // Visual indicator: blink GREEN LED every 10 seconds
                if ((now - last_blink_time) >= 10000000LL) {
                    gpio_set_level(LED_GREEN_GPIO, 0);  // Turn on (active-low)
                    vTaskDelay(pdMS_TO_TICKS(200));
                    gpio_set_level(LED_GREEN_GPIO, 1);  // Turn off
                    last_blink_time = now;
                }

                // Log countdown every 30 seconds
                if ((now - last_log_time) >= 30000000LL) {
                    ESP_LOGI(TAG, "Auto-sleep countdown: %ld seconds remaining", remaining_sec);
                    last_log_time = now;
                }
            } else {
                // Time to sleep
                ESP_LOGI(TAG, "Sleep timeout reached, entering deep sleep");
                power_manager_enter_sleep();
            }
        } else {
            // Deep sleep disabled - reset timer to prevent it from triggering
            next_sleep_time = 0;
        }
    }
}

static void power_manager_enable_auto_light_sleep(void)
{
    // Configure automatic light sleep with CPU frequency scaling
    // This allows the ESP32 to automatically enter light sleep when idle
    // and scale CPU frequency down to save power while maintaining WiFi connectivity
    esp_pm_config_t pm_config = {
        .max_freq_mhz = 160,         // Maximum CPU frequency (160MHz for ESP32-S3)
        .min_freq_mhz = 40,          // Minimum CPU frequency (40MHz when idle)
        .light_sleep_enable = true,  // Enable automatic light sleep
    };

    esp_err_t pm_ret = esp_pm_configure(&pm_config);
    if (pm_ret == ESP_OK) {
        ESP_LOGI(TAG, "Automatic light sleep enabled (CPU: 160MHz -> 40MHz)");
    } else {
        ESP_LOGW(TAG, "Failed to configure power management: %s", esp_err_to_name(pm_ret));
    }
}

static void power_manager_disable_auto_light_sleep(void)
{
    esp_pm_config_t pm_config = {
        .max_freq_mhz = 160,  // Maximum CPU frequency (160MHz for ESP32-S3)
        .min_freq_mhz = 40,   // Minimum CPU frequency (40MHz when idle)
        .light_sleep_enable = false,
    };

    esp_err_t pm_ret = esp_pm_configure(&pm_config);
    if (pm_ret == ESP_OK) {
        ESP_LOGI(TAG, "Automatic light sleep disabled");
    } else {
        ESP_LOGW(TAG, "Failed to configure power management: %s", esp_err_to_name(pm_ret));
    }

    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
}

esp_err_t power_manager_init(void)
{
    // Load deep sleep enabled setting from NVS
    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle) == ESP_OK) {
        uint8_t enabled = 1;  // Default to enabled
        nvs_get_u8(nvs_handle, NVS_DEEP_SLEEP_KEY, &enabled);
        deep_sleep_enabled = (enabled != 0);
        nvs_close(nvs_handle);
    }
    ESP_LOGI(TAG, "Deep sleep %s", deep_sleep_enabled ? "enabled" : "disabled");

    // Get wakeup causes bitmap (new API in ESP-IDF v6.0)
    uint32_t wakeup_causes = esp_sleep_get_wakeup_causes();
    ext1_wakeup_pin_mask = 0;

    // Convert bitmap to single cause for backward compatibility
    if (wakeup_causes & (1 << ESP_SLEEP_WAKEUP_TIMER)) {
        last_wakeup_cause = ESP_SLEEP_WAKEUP_TIMER;
        ESP_LOGI(TAG, "Wakeup caused by timer (auto-rotate)");

        // Check time drift and force NTP sync if needed
        if (expected_wakeup_time > 0) {
            time_t now;
            time(&now);
            int drift = (int) (now - expected_wakeup_time);
            ESP_LOGI(TAG, "Wakeup time drift: %d seconds (expected: %ld, actual: %ld)", drift,
                     (long) expected_wakeup_time, (long) now);

            // If drift exceeds 30 seconds, force NTP sync
            if (drift > 30 || drift < -30) {
                ESP_LOGW(TAG, "Time drift exceeds 30s, will force NTP sync");
                periodic_tasks_force_run(SNTP_TASK_NAME);
            }
            expected_wakeup_time = 0;  // Reset after checking
        }
    } else if (wakeup_causes & (1 << ESP_SLEEP_WAKEUP_EXT1)) {
        last_wakeup_cause = ESP_SLEEP_WAKEUP_EXT1;
        // ESP32-S3 only supports EXT1, check which GPIO triggered it
        ext1_wakeup_pin_mask = esp_sleep_get_ext1_wakeup_status();

        if (BOARD_HAL_WAKEUP_KEY != GPIO_NUM_NC &&
            (ext1_wakeup_pin_mask & (1ULL << BOARD_HAL_WAKEUP_KEY))) {
            ESP_LOGI(TAG, "Wakeup caused by BOOT button (GPIO %d)", BOARD_HAL_WAKEUP_KEY);
        } else if (BOARD_HAL_ROTATE_KEY != GPIO_NUM_NC &&
                   (ext1_wakeup_pin_mask & (1ULL << BOARD_HAL_ROTATE_KEY))) {
            ESP_LOGI(TAG, "Wakeup caused by KEY button (GPIO %d)", BOARD_HAL_ROTATE_KEY);
        } else {
            ESP_LOGI(TAG, "Wakeup caused by EXT1 (unknown GPIO: 0x%llx)", ext1_wakeup_pin_mask);
        }
    } else {
        last_wakeup_cause = ESP_SLEEP_WAKEUP_UNDEFINED;
        ESP_LOGI(TAG, "Not a deep sleep wakeup");
    }

    // Configure button GPIOs as input with pull-ups
    uint64_t pin_mask = 0;
    if (BOARD_HAL_WAKEUP_KEY != GPIO_NUM_NC) {
        pin_mask |= (1ULL << BOARD_HAL_WAKEUP_KEY);
    }
    if (BOARD_HAL_ROTATE_KEY != GPIO_NUM_NC) {
        pin_mask |= (1ULL << BOARD_HAL_ROTATE_KEY);
    }

    if (pin_mask != 0) {
        gpio_config_t io_conf = {.intr_type = GPIO_INTR_DISABLE,
                                 .mode = GPIO_MODE_INPUT,
                                 .pin_bit_mask = pin_mask,
                                 .pull_down_en = GPIO_PULLDOWN_DISABLE,
                                 .pull_up_en = GPIO_PULLUP_ENABLE};
        gpio_config(&io_conf);

        // Hold GPIO state during deep sleep to prevent floating
        // This prevents false EXT1 wake-ups when timer fires
        if (BOARD_HAL_WAKEUP_KEY != GPIO_NUM_NC) {
            gpio_hold_en(BOARD_HAL_WAKEUP_KEY);
        }
        if (BOARD_HAL_ROTATE_KEY != GPIO_NUM_NC) {
            gpio_hold_en(BOARD_HAL_ROTATE_KEY);
        }
        gpio_deep_sleep_hold_en();
    }

    // Configure LED GPIOs as output and turn them off
    gpio_config_t led_conf = {.intr_type = GPIO_INTR_DISABLE,
                              .mode = GPIO_MODE_OUTPUT,
                              .pin_bit_mask = (1ULL << LED_RED_GPIO) | (1ULL << LED_GREEN_GPIO),
                              .pull_down_en = GPIO_PULLDOWN_DISABLE,
                              .pull_up_en = GPIO_PULLUP_DISABLE};
    gpio_config(&led_conf);

    // Turn on red LED only if deep sleep is enabled (to indicate battery mode)
    // If deep sleep is disabled, keep LED off to save battery
    gpio_set_level(LED_RED_GPIO, deep_sleep_enabled ? 0 : 1);  // active-low
    gpio_set_level(LED_GREEN_GPIO, 1);                         // Turn off green LED (active-low)

    xTaskCreate(sleep_timer_task, "sleep_timer", 4096, NULL, 5, &sleep_timer_task_handle);
    xTaskCreate(rotation_timer_task, "rotation_timer", 16384, NULL, 5, &rotation_timer_task_handle);

    power_manager_enable_auto_light_sleep();

    ESP_LOGI(TAG, "Power manager initialized");
    return ESP_OK;
}

void power_manager_enter_sleep(void)
{
    power_manager_disable_auto_light_sleep();

    ESP_LOGI(TAG, "Preparing to enter deep sleep mode");

    ha_notify_offline();

    // Turn off LEDs before sleep to save power (active-low)
    gpio_set_level(LED_RED_GPIO, 1);
    gpio_set_level(LED_GREEN_GPIO, 1);

    // Check if auto-rotate is enabled
    if (config_manager_get_auto_rotate()) {
        // Use timer-based sleep for auto-rotate
        int wake_seconds = get_next_rotation_interval();

        ESP_LOGI(TAG, "Auto-rotate enabled, setting timer wake-up for %d seconds (%s)",
                 wake_seconds,
                 config_manager_get_auto_rotate_aligned() ? "clock-aligned" : "interval");
        esp_sleep_enable_timer_wakeup(wake_seconds * 1000000ULL);

        // Store expected wakeup time in RTC memory for drift detection
        time_t now;
        time(&now);
        expected_wakeup_time = now + wake_seconds;
    }

    // Enable boot button and key button wake-up (ESP32-S3 only supports EXT1)
    uint64_t wakeup_mask = 0;
    if (BOARD_HAL_WAKEUP_KEY != GPIO_NUM_NC) {
        wakeup_mask |= (1ULL << BOARD_HAL_WAKEUP_KEY);
    }
    if (BOARD_HAL_ROTATE_KEY != GPIO_NUM_NC) {
        wakeup_mask |= (1ULL << BOARD_HAL_ROTATE_KEY);
    }

    if (wakeup_mask != 0) {
        esp_sleep_enable_ext1_wakeup(wakeup_mask, ESP_EXT1_WAKEUP_ANY_LOW);
    }

    ESP_LOGI(TAG, "Configuring Board HAL for deep sleep");
    board_hal_prepare_for_sleep();

    ESP_LOGI(TAG, "Entering deep sleep now");
    vTaskDelay(pdMS_TO_TICKS(100));

    esp_deep_sleep_start();
}

void power_manager_reset_sleep_timer(void)
{
    next_sleep_time = esp_timer_get_time() + (AUTO_SLEEP_TIMEOUT_SEC * 1000000LL);
}

void power_manager_reset_rotate_timer(void)
{
    int seconds_until_next = get_next_rotation_interval();

    next_rotation_time = esp_timer_get_time() + (seconds_until_next * 1000000LL);
    ESP_LOGI(TAG, "Rotation timer reset, next rotation in %d seconds (%s)", seconds_until_next,
             config_manager_get_auto_rotate_aligned() ? "clock-aligned" : "interval");
}

bool power_manager_is_timer_wakeup(void)
{
    return last_wakeup_cause == ESP_SLEEP_WAKEUP_TIMER;
}

bool power_manager_is_ext1_wakeup(void)
{
    return last_wakeup_cause == ESP_SLEEP_WAKEUP_EXT1;
}

bool power_manager_is_boot_button_wakeup(void)
{
    if (BOARD_HAL_WAKEUP_KEY == GPIO_NUM_NC) {
        return false;
    }
    return (last_wakeup_cause == ESP_SLEEP_WAKEUP_EXT1) &&
           (ext1_wakeup_pin_mask & (1ULL << BOARD_HAL_WAKEUP_KEY));
}

bool power_manager_is_key_button_wakeup(void)
{
    if (BOARD_HAL_ROTATE_KEY == GPIO_NUM_NC) {
        return false;
    }
    return (last_wakeup_cause == ESP_SLEEP_WAKEUP_EXT1) &&
           (ext1_wakeup_pin_mask & (1ULL << BOARD_HAL_ROTATE_KEY));
}

void power_manager_set_deep_sleep_enabled(bool enabled)
{
    deep_sleep_enabled = enabled;

    // Save to NVS
    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_u8(nvs_handle, NVS_DEEP_SLEEP_KEY, enabled ? 1 : 0);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }

    // Update RED LED state: on when deep sleep enabled, off when disabled (to save battery)
    gpio_set_level(LED_RED_GPIO, enabled ? 0 : 1);  // active-low

    ESP_LOGI(TAG, "Deep sleep %s", enabled ? "enabled" : "disabled");
}

bool power_manager_get_deep_sleep_enabled(void)
{
    return deep_sleep_enabled;
}
