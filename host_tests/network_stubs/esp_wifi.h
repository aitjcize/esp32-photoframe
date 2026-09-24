#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
typedef int wifi_mode_t;
typedef struct {
    int dummy;
} wifi_ap_record_t;
typedef struct {
    int dummy;
} wifi_scan_config_t;
typedef struct {
    int dummy;
} wifi_init_config_t;
typedef struct {
    struct {
        char ssid[32];
        char password[64];
        struct {
            int authmode;
        } threshold;
        struct {
            bool capable, required;
        } pmf_cfg;
    } sta;
} wifi_config_t;
typedef struct {
    int reason;
} wifi_event_sta_disconnected_t;
#define WIFI_INIT_CONFIG_DEFAULT() {0}
#define WIFI_MODE_STA 1
#define WIFI_MODE_AP 2
#define WIFI_MODE_APSTA 3
#define WIFI_IF_STA 0
#define WIFI_AUTH_WPA2_PSK 3
#define WIFI_PS_NONE 0
#define WIFI_PS_MIN_MODEM 1
#define WIFI_REASON_AUTH_FAIL 202
#define WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT 15
#define WIFI_REASON_HANDSHAKE_TIMEOUT 204
#define WIFI_REASON_NO_AP_FOUND 201
esp_err_t esp_wifi_init(const wifi_init_config_t *config);
esp_err_t esp_wifi_set_mode(wifi_mode_t mode);
esp_err_t esp_wifi_get_mode(wifi_mode_t *mode);
esp_err_t esp_wifi_set_config(int interface, const wifi_config_t *config);
esp_err_t esp_wifi_start(void);
esp_err_t esp_wifi_stop(void);
esp_err_t esp_wifi_connect(void);
esp_err_t esp_wifi_disconnect(void);
esp_err_t esp_wifi_set_ps(int mode);
esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *config, bool blocking);
esp_err_t esp_wifi_scan_get_ap_num(uint16_t *num);
esp_err_t esp_wifi_scan_get_ap_records(uint16_t *num, wifi_ap_record_t *records);

#define ESP_ERR_WIFI_NOT_INIT 0x3001
#define ESP_ERR_WIFI_NOT_STARTED 0x3002
#define ESP_ERR_WIFI_NOT_CONNECT 0x300f
