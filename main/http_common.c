#include "http_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "esp_log.h"
#include "esp_wifi_types.h"
#include "wifi_manager.h"

static const char *TAG = "http_common";

static const char *auth_mode_str(wifi_auth_mode_t authmode)
{
    switch (authmode) {
    case WIFI_AUTH_OPEN:
        return "Open";
    case WIFI_AUTH_WEP:
        return "WEP";
    case WIFI_AUTH_WPA_PSK:
        return "WPA";
    case WIFI_AUTH_WPA2_PSK:
        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:
        return "WPA/WPA2";
    case WIFI_AUTH_WPA3_PSK:
        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:
        return "WPA2/WPA3";
    default:
        return "Unknown";
    }
}

#define MAX_SCAN_RESULTS 20

esp_err_t http_common_wifi_scan_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "WiFi scan requested");

    wifi_ap_record_t *ap_records = malloc(sizeof(wifi_ap_record_t) * MAX_SCAN_RESULTS);
    if (!ap_records) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    int count = wifi_manager_scan(ap_records, MAX_SCAN_RESULTS);

    // Deduplicate by SSID, keeping the strongest signal seen for each.
    int unique_count = 0;
    for (int i = 0; i < count; i++) {
        if (ap_records[i].ssid[0] == '\0') {
            continue;  // skip hidden networks
        }

        bool duplicate = false;
        for (int j = 0; j < unique_count; j++) {
            if (strcmp((char *) ap_records[j].ssid, (char *) ap_records[i].ssid) == 0) {
                if (ap_records[i].rssi > ap_records[j].rssi) {
                    ap_records[j] = ap_records[i];
                }
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            if (unique_count != i) {
                ap_records[unique_count] = ap_records[i];
            }
            unique_count++;
        }
    }

    // Sort by RSSI strongest-first (small array — bubble sort is fine).
    for (int i = 0; i < unique_count - 1; i++) {
        for (int j = 0; j < unique_count - i - 1; j++) {
            if (ap_records[j].rssi < ap_records[j + 1].rssi) {
                wifi_ap_record_t temp = ap_records[j];
                ap_records[j] = ap_records[j + 1];
                ap_records[j + 1] = temp;
            }
        }
    }

    // Build JSON response. Each entry is roughly:
    //   {"ssid":"...","rssi":-xx,"auth":"..."}  ~ 80 chars
    size_t buf_size = unique_count * 80 + 16;
    char *json_buf = malloc(buf_size);
    if (!json_buf) {
        free(ap_records);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    int pos = 0;
    pos += snprintf(json_buf + pos, buf_size - pos, "[");

    for (int i = 0; i < unique_count; i++) {
        if (i > 0) {
            pos += snprintf(json_buf + pos, buf_size - pos, ",");
        }
        // Escape SSID for JSON (handle quotes and backslashes).
        char escaped_ssid[66] = {0};
        int esc_pos = 0;
        for (int k = 0; ap_records[i].ssid[k] && esc_pos < (int) sizeof(escaped_ssid) - 2; k++) {
            char c = (char) ap_records[i].ssid[k];
            if (c == '"' || c == '\\') {
                escaped_ssid[esc_pos++] = '\\';
            }
            escaped_ssid[esc_pos++] = c;
        }
        escaped_ssid[esc_pos] = '\0';

        pos += snprintf(json_buf + pos, buf_size - pos,
                        "{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":\"%s\"}", escaped_ssid,
                        ap_records[i].rssi, auth_mode_str(ap_records[i].authmode));
    }

    pos += snprintf(json_buf + pos, buf_size - pos, "]");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_buf, pos);

    free(json_buf);
    free(ap_records);

    ESP_LOGI(TAG, "WiFi scan returned %d unique networks", unique_count);
    return ESP_OK;
}
