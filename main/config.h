#ifndef CONFIG_H
#define CONFIG_H

#include <driver/gpio.h>

#define BOOT_BUTTON_GPIO GPIO_NUM_0
#define PWR_BUTTON_GPIO GPIO_NUM_5
#define KEY_BUTTON_GPIO GPIO_NUM_4

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
#define DEFAULT_BRIGHTNESS_FSTOP 0.0  // Default brightness adjustment in f-stops
#define DEFAULT_CONTRAST 1.2          // Default contrast multiplier (1.0 = no change)

#define NVS_NAMESPACE "photoframe"
#define NVS_WIFI_SSID_KEY "wifi_ssid"
#define NVS_WIFI_PASS_KEY "wifi_pass"
#define NVS_ROTATE_INTERVAL_KEY "rotate_int"
#define NVS_AUTO_ROTATE_KEY "auto_rot"
#define NVS_AUTO_ROTATE_INDEX_KEY "auto_idx"
#define NVS_CURRENT_IMAGE_KEY "curr_img"
#define NVS_BRIGHTNESS_FSTOP_KEY "bright_fs"
#define NVS_CONTRAST_KEY "contrast"

#endif
