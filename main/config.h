#ifndef CONFIG_H
#define CONFIG_H

#include <driver/gpio.h>

#define BOOT_BUTTON_GPIO GPIO_NUM_0
#define PWR_BUTTON_GPIO GPIO_NUM_5
#define KEY_BUTTON_GPIO GPIO_NUM_4

#define LED_RED_GPIO GPIO_NUM_45
#define LED_GREEN_GPIO GPIO_NUM_42

#define WIFI_SSID_MAX_LEN 32
#define WIFI_PASS_MAX_LEN 64

#define DEFAULT_WIFI_SSID "PhotoFrame"
#define DEFAULT_WIFI_PASSWORD "photoframe123"

#define SDCARD_MOUNT_POINT "/sdcard"
#define IMAGE_DIRECTORY "/sdcard/images"

#define DISPLAY_WIDTH 800
#define DISPLAY_HEIGHT 480

#define AUTO_SLEEP_TIMEOUT_SEC 120
#define IMAGE_ROTATE_INTERVAL_SEC 3600

#define NVS_NAMESPACE "photoframe"
#define NVS_WIFI_SSID_KEY "wifi_ssid"
#define NVS_WIFI_PASS_KEY "wifi_pass"
#define NVS_ROTATE_INTERVAL_KEY "rotate_int"
#define NVS_AUTO_ROTATE_KEY "auto_rot"
#define NVS_CURRENT_IMAGE_KEY "curr_img"
#define NVS_DEEP_SLEEP_KEY "deep_sleep"

#endif
