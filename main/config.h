#ifndef CONFIG_H
#define CONFIG_H

#include <driver/gpio.h>

// Uncomment to debug deep sleep wake
// #define DEBUG_DEEP_SLEEP_WAKE

typedef enum { ROTATION_MODE_SDCARD = 0, ROTATION_MODE_URL = 1 } rotation_mode_t;

#define BOOT_BUTTON_GPIO GPIO_NUM_0
#define PWR_BUTTON_GPIO GPIO_NUM_5
#define KEY_BUTTON_GPIO GPIO_NUM_4

#define LED_RED_GPIO GPIO_NUM_45
#define LED_GREEN_GPIO GPIO_NUM_42

#define WIFI_SSID_MAX_LEN 32
#define WIFI_PASS_MAX_LEN 64
#define IMAGE_URL_MAX_LEN 256
#define HA_URL_MAX_LEN 256
#define ROTATION_MODE_MAX_LEN 16

#define DEFAULT_WIFI_SSID "PhotoFrame"
#define DEFAULT_WIFI_PASSWORD "photoframe123"
#define DEFAULT_IMAGE_URL "https://loremflickr.com/800/480"
#define DEFAULT_HA_URL ""

#define SDCARD_MOUNT_POINT "/sdcard"
#define IMAGE_DIRECTORY "/sdcard/images"
#define DEFAULT_ALBUM_NAME "Default"

#define CURRENT_JPG_PATH "/sdcard/.current.jpg"
#define CURRENT_BMP_PATH "/sdcard/.current.bmp"

#define DISPLAY_WIDTH 800
#define DISPLAY_HEIGHT 480

#ifdef DEBUG_DEEP_SLEEP_WAKE
#define AUTO_SLEEP_TIMEOUT_SEC 20
#else
#define AUTO_SLEEP_TIMEOUT_SEC 120
#endif

#define IMAGE_ROTATE_INTERVAL_SEC 3600
#define IMAGE_ROTATION_DEG 180

#define NVS_NAMESPACE "photoframe"
#define NVS_WIFI_SSID_KEY "wifi_ssid"
#define NVS_WIFI_PASS_KEY "wifi_pass"
#define NVS_ROTATE_INTERVAL_KEY "rotate_int"
#define NVS_AUTO_ROTATE_KEY "auto_rotate"
#define NVS_IMAGE_ROTATION_KEY "image_rotation"
#define NVS_BRIGHTNESS_KEY "brightness"
#define NVS_CONTRAST_KEY "contrast"
#define NVS_DEEP_SLEEP_KEY "deep_sleep"
#define NVS_ENABLED_ALBUMS_KEY "enabled_albums"
#define NVS_IMAGE_URL_KEY "image_url"
#define NVS_ROTATION_MODE_KEY "rotation_mode"
#define NVS_SAVE_DOWNLOADED_KEY "save_dl"
#define NVS_HA_URL_KEY "ha_url"

#endif
