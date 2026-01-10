#include "power_manager.h"

#include "axp_prot.h"
#include "config.h"
#include "display_manager.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "http_server.h"
#include "nvs.h"
#include "remote_gallery.h"

static const char *TAG = "power_manager";

static TaskHandle_t sleep_timer_task_handle = NULL;
static TaskHandle_t rotation_timer_task_handle = NULL;
static int64_t next_sleep_time = 0;     // Use absolute time for sleep timer
static bool deep_sleep_enabled = true;  // Enabled by default, can be disabled for HA integration
static esp_sleep_wakeup_cause_t last_wakeup_cause = ESP_SLEEP_WAKEUP_UNDEFINED;
static int64_t next_rotation_time = 0;  // Use absolute time for rotation
static uint64_t ext1_wakeup_pin_mask = 0;

static void rotation_timer_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        // Run active rotation when:
        // 1. USB is connected (device stays awake), OR
        // 2. Deep sleep is disabled (device stays awake on battery)
        bool should_use_active_rotation = axp_is_usb_connected() || !deep_sleep_enabled;

        if (!should_use_active_rotation) {
            next_rotation_time = 0;
            continue;
        }

        if (display_manager_get_auto_rotate()) {
            int64_t now = esp_timer_get_time();

            if (next_rotation_time == 0) {
                int rotate_interval = display_manager_get_rotate_interval();
                next_rotation_time = now + (rotate_interval * 1000000LL);
                const char *reason = axp_is_usb_connected() ? "USB powered" : "deep sleep disabled";
                ESP_LOGI(TAG, "Active rotation scheduled in %d seconds (%s)", rotate_interval,
                         reason);
            } else if (now >= next_rotation_time) {
                uint8_t display_mode = DISPLAY_MODE_LOCAL;
                nvs_handle_t nvs_handle;
                if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle) == ESP_OK) {
                    nvs_get_u8(nvs_handle, NVS_DISPLAY_MODE_KEY, &display_mode);
                    nvs_close(nvs_handle);
                }

                uint8_t effective_mode = display_mode;

                if (display_mode == DISPLAY_MODE_REMOTE) {
                    ESP_LOGI(TAG, "Remote gallery mode - downloading image");
                    esp_err_t download_result = remote_gallery_download_image();

                    if (download_result == ESP_OK) {
                        ESP_LOGI(TAG, "Remote image downloaded successfully");
                        effective_mode = DISPLAY_MODE_REMOTE;
                    } else {
                        ESP_LOGW(TAG, "Remote download failed, falling back to local gallery");
                        effective_mode = DISPLAY_MODE_LOCAL;
                    }
                }

                const char *reason = axp_is_usb_connected() ? "USB powered" : "deep sleep disabled";
                ESP_LOGI(TAG, "Active rotation triggered (%s), mode=%d", reason, effective_mode);
                display_manager_handle_wakeup(effective_mode);

                int rotate_interval = display_manager_get_rotate_interval();
                next_rotation_time = now + (rotate_interval * 1000000LL);
                ESP_LOGI(TAG, "Next rotation scheduled in %d seconds", rotate_interval);
            }
        } else {
            next_rotation_time = 0;
        }
    }
}

static void sleep_timer_task(void *arg)
{
    int64_t last_blink_time = 0;
    int64_t last_log_time = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (axp_is_usb_connected()) {
            next_sleep_time = 0;
            continue;
        }

        if (deep_sleep_enabled) {
            int64_t now = esp_timer_get_time();

            if (next_sleep_time == 0) {
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
    } else if (wakeup_causes & (1 << ESP_SLEEP_WAKEUP_EXT1)) {
        last_wakeup_cause = ESP_SLEEP_WAKEUP_EXT1;
        // ESP32-S3 only supports EXT1, check which GPIO triggered it
        ext1_wakeup_pin_mask = esp_sleep_get_ext1_wakeup_status();

        if (ext1_wakeup_pin_mask & (1ULL << BOOT_BUTTON_GPIO)) {
            ESP_LOGI(TAG, "Wakeup caused by BOOT button (GPIO %d)", BOOT_BUTTON_GPIO);
        } else if (ext1_wakeup_pin_mask & (1ULL << KEY_BUTTON_GPIO)) {
            ESP_LOGI(TAG, "Wakeup caused by KEY button (GPIO %d)", KEY_BUTTON_GPIO);
        } else {
            ESP_LOGI(TAG, "Wakeup caused by EXT1 (unknown GPIO: 0x%llx)", ext1_wakeup_pin_mask);
        }
    } else {
        last_wakeup_cause = ESP_SLEEP_WAKEUP_UNDEFINED;
        ESP_LOGI(TAG, "Not a deep sleep wakeup");
    }

    // Configure button GPIOs as input with pull-ups
    // NOTE: PWR_BUTTON_GPIO (GPIO 5) is NOT configured here - it's AXP2101 SYS_OUT pin
    gpio_config_t io_conf = {.intr_type = GPIO_INTR_DISABLE,
                             .mode = GPIO_MODE_INPUT,
                             .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO) | (1ULL << KEY_BUTTON_GPIO),
                             .pull_down_en = GPIO_PULLDOWN_DISABLE,
                             .pull_up_en = GPIO_PULLUP_ENABLE};
    gpio_config(&io_conf);

    // Hold GPIO state during deep sleep to prevent floating
    // This prevents false EXT1 wake-ups when timer fires
    gpio_hold_en(BOOT_BUTTON_GPIO);
    gpio_hold_en(KEY_BUTTON_GPIO);
    gpio_deep_sleep_hold_en();

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
    xTaskCreate(rotation_timer_task, "rotation_timer", 4096, NULL, 5, &rotation_timer_task_handle);

    power_manager_enable_auto_light_sleep();

    ESP_LOGI(TAG, "Power manager initialized");
    return ESP_OK;
}

void power_manager_enter_sleep(void)
{
    power_manager_disable_auto_light_sleep();

    ESP_LOGI(TAG, "Preparing to enter deep sleep mode");

    // Turn off LEDs before sleep to save power (active-low)
    gpio_set_level(LED_RED_GPIO, 1);
    gpio_set_level(LED_GREEN_GPIO, 1);

    // Check if auto-rotate is enabled
    if (display_manager_get_auto_rotate()) {
        // Use timer-based sleep for auto-rotate
        int rotate_interval = display_manager_get_rotate_interval();
        ESP_LOGI(TAG, "Auto-rotate enabled, setting timer wake-up for %d seconds", rotate_interval);
        esp_sleep_enable_timer_wakeup(rotate_interval * 1000000ULL);
    }

    // Enable boot button and key button wake-up (ESP32-S3 only supports EXT1)
    esp_sleep_enable_ext1_wakeup((1ULL << BOOT_BUTTON_GPIO) | (1ULL << KEY_BUTTON_GPIO),
                                 ESP_EXT1_WAKEUP_ANY_LOW);

    ESP_LOGI(TAG, "Configuring AXP2101 for deep sleep");
    axp_basic_sleep_start();

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
    int rotate_interval = display_manager_get_rotate_interval();
    next_rotation_time = esp_timer_get_time() + (rotate_interval * 1000000LL);
    ESP_LOGI(TAG, "Rotation timer reset, next rotation in %d seconds", rotate_interval);
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
    return (last_wakeup_cause == ESP_SLEEP_WAKEUP_EXT1) &&
           (ext1_wakeup_pin_mask & (1ULL << BOOT_BUTTON_GPIO));
}

bool power_manager_is_key_button_wakeup(void)
{
    return (last_wakeup_cause == ESP_SLEEP_WAKEUP_EXT1) &&
           (ext1_wakeup_pin_mask & (1ULL << KEY_BUTTON_GPIO));
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
