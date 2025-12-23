#include "power_manager.h"

#include "axp_prot.h"
#include "ble_wake_service.h"
#include "config.h"
#include "display_manager.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "power_manager";

static TaskHandle_t sleep_timer_task_handle = NULL;
static TaskHandle_t rotation_timer_task_handle = NULL;
static uint32_t sleep_countdown = AUTO_SLEEP_TIMEOUT_SEC;
static bool sleep_enabled = true;  // Enabled by default to prevent battery drain
static esp_sleep_wakeup_cause_t last_wakeup_cause = ESP_SLEEP_WAKEUP_UNDEFINED;
static uint32_t rotate_countdown = 0;
static uint64_t ext1_wakeup_pin_mask = 0;

static void rotation_timer_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        // Run active rotation when:
        // 1. USB is connected (device stays awake), OR
        // 2. On battery with BLE wake enabled (automatic light sleep keeps device responsive)
        bool should_run = axp_is_usb_connected() || ble_wake_service_get_enabled();
        
        if (!should_run) {
            rotate_countdown = 0;  // Reset when on battery without BLE (uses sleep-based rotation)
            continue;
        }

        // Handle active rotation when conditions are met and auto-rotate enabled
        if (display_manager_get_auto_rotate()) {
            if (rotate_countdown == 0) {
                // Initialize rotation countdown
                rotate_countdown = display_manager_get_rotate_interval();
            } else {
                rotate_countdown--;
                if (rotate_countdown == 0) {
                    ESP_LOGI(TAG, "Active rotation triggered");
                    display_manager_handle_timer_wakeup();
                    rotate_countdown = display_manager_get_rotate_interval();
                }
            }
        } else {
            rotate_countdown = 0;  // Reset if auto-rotate disabled
        }
    }
}

static void sleep_timer_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        // Skip auto-sleep when USB is connected
        if (axp_is_usb_connected()) {
            // Reset countdown so it doesn't trigger immediately when USB is unplugged
            sleep_countdown = AUTO_SLEEP_TIMEOUT_SEC;
            continue;
        }

        // Handle auto-sleep countdown when on battery
        if (sleep_enabled && sleep_countdown > 0) {
            sleep_countdown--;

            // Visual indicator: blink GREEN LED every 10 seconds to show countdown is active
            if (sleep_countdown % 10 == 0) {
                gpio_set_level(LED_GREEN_GPIO, 0);  // Turn on (active-low)
                vTaskDelay(pdMS_TO_TICKS(200));
                gpio_set_level(LED_GREEN_GPIO, 1);  // Turn off
            }

            // Log countdown every 30 seconds
            if (sleep_countdown % 30 == 0 && sleep_countdown > 0) {
                ESP_LOGI(TAG, "Auto-sleep countdown: %lu seconds remaining", sleep_countdown);
            }

            if (sleep_countdown == 0) {
                ESP_LOGI(TAG, "Sleep timeout reached, entering deep sleep");
                power_manager_enter_sleep();
            }
        }
    }
}

esp_err_t power_manager_init(void)
{
    uint32_t wakeup_causes = esp_sleep_get_wakeup_causes();
    ext1_wakeup_pin_mask = 0;

    // Check wakeup causes directly
    if (wakeup_causes & ESP_SLEEP_WAKEUP_TIMER) {
        ESP_LOGI(TAG, "Wakeup caused by timer (auto-rotate)");
        last_wakeup_cause = ESP_SLEEP_WAKEUP_TIMER;
        // Disable auto-sleep for timer wakeup - device will go back to sleep immediately
        sleep_enabled = false;
    } else if (wakeup_causes & ESP_SLEEP_WAKEUP_EXT1) {
        // ESP32-S3 only supports EXT1, check which GPIO triggered it
        ext1_wakeup_pin_mask = esp_sleep_get_ext1_wakeup_status();
        last_wakeup_cause = ESP_SLEEP_WAKEUP_EXT1;

        if (ext1_wakeup_pin_mask & (1ULL << BOOT_BUTTON_GPIO)) {
            ESP_LOGI(TAG, "Wakeup caused by BOOT button (GPIO %d)", BOOT_BUTTON_GPIO);
        } else if (ext1_wakeup_pin_mask & (1ULL << KEY_BUTTON_GPIO)) {
            ESP_LOGI(TAG, "Wakeup caused by KEY button (GPIO %d)", KEY_BUTTON_GPIO);
        } else {
            ESP_LOGI(TAG, "Wakeup caused by EXT1 (unknown GPIO: 0x%llx)", ext1_wakeup_pin_mask);
        }
    } else if (wakeup_causes & ESP_SLEEP_WAKEUP_BT) {
        ESP_LOGI(TAG, "Wakeup caused by BLE");
        last_wakeup_cause = ESP_SLEEP_WAKEUP_BT;
    } else {
        ESP_LOGI(TAG, "Not a deep sleep wakeup");
        last_wakeup_cause = ESP_SLEEP_WAKEUP_UNDEFINED;
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
    gpio_set_level(LED_RED_GPIO, 0);    // Turn on red LED when awake (active-low)
    gpio_set_level(LED_GREEN_GPIO, 1);  // Turn off green LED (active-low)

    xTaskCreate(sleep_timer_task, "sleep_timer", 4096, NULL, 5, &sleep_timer_task_handle);
    xTaskCreate(rotation_timer_task, "rotation_timer", 4096, NULL, 5, &rotation_timer_task_handle);

    ESP_LOGI(TAG, "Power manager initialized");
    return ESP_OK;
}

void power_manager_enter_sleep(void)
{
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

    // Check if BLE wake mode is enabled
    if (ble_wake_service_get_enabled()) {
        ESP_LOGI(TAG, "BLE wake enabled - automatic light sleep is active (system will sleep/wake automatically)");
        // With automatic light sleep enabled, the system handles sleep automatically
        // BLE remains active and the device will wake when needed
        return;
    } else {
        ESP_LOGI(TAG, "Entering deep sleep now");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_deep_sleep_start();
    }
}

void power_manager_enter_sleep_with_timer(uint32_t sleep_time_sec)
{
    ESP_LOGI(TAG, "Preparing to enter deep sleep with timer wake-up in %lu seconds",
             sleep_time_sec);

    // Turn off LEDs before sleep to save power (active-low)
    gpio_set_level(LED_RED_GPIO, 1);
    gpio_set_level(LED_GREEN_GPIO, 1);

    // Enable timer, boot button, and key button wake-up (ESP32-S3 only supports EXT1)
    esp_sleep_enable_timer_wakeup(sleep_time_sec * 1000000ULL);  // Convert to microseconds
    esp_sleep_enable_ext1_wakeup((1ULL << BOOT_BUTTON_GPIO) | (1ULL << KEY_BUTTON_GPIO),
                                 ESP_EXT1_WAKEUP_ANY_LOW);

    // Check if BLE wake mode is enabled
    if (ble_wake_service_get_enabled()) {
        ESP_LOGI(TAG, "BLE wake enabled - automatic light sleep is active (system will sleep/wake automatically)");
        // With automatic light sleep enabled, the system handles sleep automatically
        // BLE remains active and the device will wake when needed
        return;
    } else {
        ESP_LOGI(TAG, "Entering deep sleep now");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_deep_sleep_start();
    }
}

void power_manager_trigger_sleep(void)
{
    ESP_LOGI(TAG, "Sleep triggered");

    // Check if auto-rotate is enabled and use its interval for wake-up
    if (display_manager_get_auto_rotate()) {
        power_manager_enter_sleep_with_timer(display_manager_get_rotate_interval());
    } else {
        power_manager_enter_sleep();
    }
}

void power_manager_reset_sleep_timer(void)
{
    sleep_countdown = AUTO_SLEEP_TIMEOUT_SEC;
    sleep_enabled = true;  // Enable sleep timer when user interacts with web server
}

void power_manager_reset_rotate_timer(void)
{
    rotate_countdown = display_manager_get_rotate_interval();
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
