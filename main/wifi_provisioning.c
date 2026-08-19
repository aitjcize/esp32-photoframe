#include "wifi_provisioning.h"

#include <string.h>

#include "config.h"
#include "config_manager.h"
#include "dns_server.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "power_manager.h"
#include "utils.h"
#include "wifi_manager.h"

static const char *TAG = "wifi_prov";
static httpd_handle_t provisioning_server = NULL;

// Structure to pass credentials to test task
typedef struct {
    char ssid[WIFI_SSID_MAX_LEN];
    char password[WIFI_PASS_MAX_LEN];
} wifi_test_params_t;

// Webapp assets - same as http_server.c
extern const uint8_t index_html_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_gz_end");
extern const uint8_t index_css_start[] asm("_binary_index_css_gz_start");
extern const uint8_t index_css_end[] asm("_binary_index_css_gz_end");
extern const uint8_t index_js_start[] asm("_binary_index_js_gz_start");
extern const uint8_t index_js_end[] asm("_binary_index_js_gz_end");
extern const uint8_t index2_js_start[] asm("_binary_index2_js_gz_start");
extern const uint8_t index2_js_end[] asm("_binary_index2_js_gz_end");
extern const uint8_t exif_reader_js_start[] asm("_binary_exif_reader_js_gz_start");
extern const uint8_t exif_reader_js_end[] asm("_binary_exif_reader_js_gz_end");
extern const uint8_t browser_js_start[] asm("_binary_browser_js_gz_start");
extern const uint8_t browser_js_end[] asm("_binary_browser_js_gz_end");
extern const uint8_t vite_browser_external_js_start[] asm(
    "_binary___vite_browser_external_js_gz_start");
extern const uint8_t vite_browser_external_js_end[] asm(
    "_binary___vite_browser_external_js_gz_end");
extern const uint8_t icon_svg_start[] asm("_binary_icon_svg_gz_start");
extern const uint8_t icon_svg_end[] asm("_binary_icon_svg_gz_end");

static esp_err_t provision_keep_alive_handler(httpd_req_t *req)
{
    power_manager_reset_sleep_timer();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t provision_index_handler(httpd_req_t *req)
{
    // If accessing root, redirect to /provision
    if (strcmp(req->uri, "/") == 0) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/provision");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    const size_t index_html_size = (index_html_end - index_html_start);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_send(req, (const char *) index_html_start, index_html_size);
    return ESP_OK;
}

static esp_err_t provision_css_handler(httpd_req_t *req)
{
    const size_t index_css_size = (index_css_end - index_css_start);
    httpd_resp_set_type(req, "text/css");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_send(req, (const char *) index_css_start, index_css_size);
    return ESP_OK;
}

static esp_err_t provision_js_handler(httpd_req_t *req)
{
    const size_t index_js_size = (index_js_end - index_js_start);
    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_send(req, (const char *) index_js_start, index_js_size);
    return ESP_OK;
}

static esp_err_t provision_js2_handler(httpd_req_t *req)
{
    const size_t index2_js_size = (index2_js_end - index2_js_start);
    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_send(req, (const char *) index2_js_start, index2_js_size);
    return ESP_OK;
}

static esp_err_t provision_exif_js_handler(httpd_req_t *req)
{
    const size_t exif_reader_js_size = (exif_reader_js_end - exif_reader_js_start);
    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_send(req, (const char *) exif_reader_js_start, exif_reader_js_size);
    return ESP_OK;
}

static esp_err_t provision_browser_js_handler(httpd_req_t *req)
{
    const size_t browser_js_size = (browser_js_end - browser_js_start);
    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_send(req, (const char *) browser_js_start, browser_js_size);
    return ESP_OK;
}

static esp_err_t provision_vite_js_handler(httpd_req_t *req)
{
    const size_t vite_js_size = (vite_browser_external_js_end - vite_browser_external_js_start);
    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_send(req, (const char *) vite_browser_external_js_start, vite_js_size);
    return ESP_OK;
}

static esp_err_t provision_icon_handler(httpd_req_t *req)
{
    const size_t icon_svg_size = (icon_svg_end - icon_svg_start);
    httpd_resp_set_type(req, "image/svg+xml");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_send(req, (const char *) icon_svg_start, icon_svg_size);
    return ESP_OK;
}

// Handler for captive portal detection URLs
static esp_err_t captive_portal_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Captive portal detection request: %s", req->uri);

    // Redirect to /provision route
    const char *success_response =
        "<!DOCTYPE html><html><head>"
        "<meta http-equiv='refresh' content='0;url=http://192.168.4.1/provision'>"
        "</head><body>Success</body></html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, success_response, strlen(success_response));
    return ESP_OK;
}

// Error handler for 404 - acts as catch-all
static esp_err_t captive_portal_error_handler(httpd_req_t *req, httpd_err_code_t err)
{
    ESP_LOGI(TAG, "404 catch-all request: %s", req->uri);

    // Redirect to /provision route
    const char *success_response =
        "<!DOCTYPE html><html><head>"
        "<meta http-equiv='refresh' content='0;url=http://192.168.4.1/provision'>"
        "</head><body>Redirecting...</body></html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_send(req, success_response, strlen(success_response));
    return ESP_OK;
}

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

static esp_err_t provision_scan_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "WiFi scan requested");

#define MAX_SCAN_RESULTS 20
    wifi_ap_record_t *ap_records = malloc(sizeof(wifi_ap_record_t) * MAX_SCAN_RESULTS);
    if (!ap_records) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    int count = wifi_manager_scan(ap_records, MAX_SCAN_RESULTS);

    // Deduplicate by SSID, keeping strongest signal for each
    // Use in-place dedup: for each entry, check if SSID already seen earlier
    int unique_count = 0;
    for (int i = 0; i < count; i++) {
        // Skip hidden networks (empty SSID)
        if (ap_records[i].ssid[0] == '\0') {
            continue;
        }

        // Check if this SSID was already added
        bool duplicate = false;
        for (int j = 0; j < unique_count; j++) {
            if (strcmp((char *) ap_records[j].ssid, (char *) ap_records[i].ssid) == 0) {
                // Keep the one with stronger signal
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

    // Sort by RSSI (strongest first) - simple bubble sort for small array
    for (int i = 0; i < unique_count - 1; i++) {
        for (int j = 0; j < unique_count - i - 1; j++) {
            if (ap_records[j].rssi < ap_records[j + 1].rssi) {
                wifi_ap_record_t temp = ap_records[j];
                ap_records[j] = ap_records[j + 1];
                ap_records[j + 1] = temp;
            }
        }
    }

    // Build JSON response
    // Each entry: {"ssid":"...", "rssi":-xx, "auth":"..."} ~ max 80 chars
    // Array overhead + commas: ~2 + unique_count
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
        // Escape SSID for JSON (handle quotes and backslashes)
        char escaped_ssid[66] = {0};  // 33 chars max SSID * 2 for escaping
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

// Extract one field from a URL-encoded form body ("key=value&..."), decoding
// '+' and %XX escapes. Returns true when the key is present (value may be
// empty). Unlike the positional parsing above, this is order-independent.
static bool get_form_field(const char *buf, const char *key, char *out, size_t out_len)
{
    size_t key_len = strlen(key);
    const char *p = buf;
    while ((p = strstr(p, key)) != NULL) {
        // Must be the start of a field: beginning of buffer or right after '&',
        // and followed by '='.
        if ((p == buf || p[-1] == '&') && p[key_len] == '=') {
            break;
        }
        p += key_len;
    }
    if (p == NULL) {
        return false;
    }

    const char *v = p + key_len + 1;
    size_t o = 0;
    while (*v != '\0' && *v != '&' && o < out_len - 1) {
        if (*v == '+') {
            out[o++] = ' ';
            v++;
        } else if (*v == '%' && v[1] != '\0' && v[2] != '\0') {
            char hex[3] = {v[1], v[2], 0};
            out[o++] = (char) strtol(hex, NULL, 16);
            v += 3;
        } else {
            out[o++] = *v++;
        }
    }
    out[o] = '\0';
    return true;
}

static esp_err_t provision_save_handler(httpd_req_t *req)
{
    char buf[512];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data received");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    char ssid[WIFI_SSID_MAX_LEN] = {0};
    char password[WIFI_PASS_MAX_LEN] = {0};
    char device_name[DEVICE_NAME_MAX_LEN] = {0};

    // Order-independent, URL-decoding field extraction. The previous
    // positional parser sliced deviceName from "&deviceName=" to the END of
    // the body, so any field appended after it (the #43 network settings) got
    // swallowed into the device name — and, via hostname sanitization, into
    // the mDNS name (e.g. "...-ipmode-dhcp.local").
    if (!get_form_field(buf, "ssid", ssid, sizeof(ssid)) || ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing SSID");
        return ESP_FAIL;
    }
    get_form_field(buf, "password", password, sizeof(password));
    get_form_field(buf, "deviceName", device_name, sizeof(device_name));

    // Use default if device name is empty
    if (strlen(device_name) == 0) {
        strncpy(device_name, DEFAULT_DEVICE_NAME, DEVICE_NAME_MAX_LEN - 1);
        device_name[DEVICE_NAME_MAX_LEN - 1] = '\0';
    }

    // Optional network settings (#43): static IP + DNS override, for networks
    // without DHCP. Validated here and applied below so the connection test
    // runs with the exact configuration that will be saved. A save without
    // ipMode=static always resets to DHCP — re-provisioning is the recovery
    // path for a broken static config.
    char ip_mode_str[8] = {0};
    char field_ip[IP_ADDR_STR_MAX_LEN] = {0};
    char field_mask[IP_ADDR_STR_MAX_LEN] = {0};
    char field_gw[IP_ADDR_STR_MAX_LEN] = {0};
    char field_dns[IP_ADDR_STR_MAX_LEN] = {0};
    get_form_field(buf, "ipMode", ip_mode_str, sizeof(ip_mode_str));
    bool want_static = (strcmp(ip_mode_str, "static") == 0);
    if (want_static) {
        esp_ip4_addr_t parsed;
        if (!get_form_field(buf, "staticIp", field_ip, sizeof(field_ip)) ||
            esp_netif_str_to_ip4(field_ip, &parsed) != ESP_OK ||
            !get_form_field(buf, "staticNetmask", field_mask, sizeof(field_mask)) ||
            esp_netif_str_to_ip4(field_mask, &parsed) != ESP_OK ||
            !get_form_field(buf, "staticGateway", field_gw, sizeof(field_gw)) ||
            esp_netif_str_to_ip4(field_gw, &parsed) != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                "Invalid static IP configuration (IP, netmask and gateway must be "
                                "valid IPv4 addresses)");
            return ESP_FAIL;
        }
    }
    if (get_form_field(buf, "dnsServer", field_dns, sizeof(field_dns)) && field_dns[0] != '\0') {
        esp_ip4_addr_t parsed;
        if (esp_netif_str_to_ip4(field_dns, &parsed) != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid DNS server address");
            return ESP_FAIL;
        }
    }

    if (want_static) {
        config_manager_set_static_ip(field_ip);
        config_manager_set_static_netmask(field_mask);
        config_manager_set_static_gateway(field_gw);
        config_manager_set_ip_mode(IP_MODE_STATIC);
    } else {
        config_manager_set_ip_mode(IP_MODE_DHCP);
    }
    config_manager_set_dns_server(field_dns);

    ESP_LOGI(TAG, "Received WiFi credentials - SSID: %s", ssid);
    ESP_LOGI(TAG, "Device name: %s", device_name);
    ESP_LOGI(TAG, "Testing WiFi connection in APSTA mode...");

    // Switch to APSTA mode to test connection while keeping AP running
    esp_wifi_set_mode(WIFI_MODE_APSTA);

    // Configure STA with provided credentials
    wifi_config_t sta_config = {0};
    strncpy((char *) sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid) - 1);
    strncpy((char *) sta_config.sta.password, password, sizeof(sta_config.sta.password) - 1);
    sta_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    sta_config.sta.pmf_cfg.capable = true;
    sta_config.sta.pmf_cfg.required = false;

    esp_wifi_set_config(WIFI_IF_STA, &sta_config);

    // Apply the just-saved IP configuration (static address or DHCP) so the
    // connection test exercises it (#43).
    wifi_manager_apply_ip_config();

    // Disconnect first if already connected
    esp_wifi_disconnect();

    // Wait a bit for disconnect to complete
    vTaskDelay(pdMS_TO_TICKS(100));

    // Try to connect
    esp_wifi_connect();

    // Wait for connection result (with timeout)
    EventBits_t bits =
        xEventGroupWaitBits(wifi_manager_get_event_group(), WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                            pdTRUE,               // Clear bits on exit
                            pdFALSE,              // Wait for either bit
                            pdMS_TO_TICKS(15000)  // 15 second timeout
        );

    if (!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGW(TAG, "Failed to connect to WiFi network: %s", ssid);

        // Connection failed - switch back to AP-only mode
        esp_wifi_disconnect();
        esp_wifi_set_mode(WIFI_MODE_AP);

        const char *error_response =
            "<html><body><h1>WiFi Connection Failed</h1>"
            "<p>Could not connect to the WiFi network. Please check your credentials and try "
            "again.</p>"
            "<p>Common issues:</p>"
            "<ul>"
            "<li>Incorrect password</li>"
            "<li>Wrong SSID (network name)</li>"
            "<li>Network is 5GHz (only 2.4GHz supported)</li>"
            "<li>Network is out of range</li>"
            "</ul>"
            "<p><a href='/'>Go Back</a></p></body></html>";
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, error_response, strlen(error_response));
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "WiFi connection successful! Saving credentials...");

    // Connection successful - save credentials
    esp_err_t err = wifi_manager_save_credentials(ssid, password);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save credentials");
        return ESP_FAIL;
    }

    // Save device name
    config_manager_set_device_name(device_name);
    ESP_LOGI(TAG, "Device name saved: %s", device_name);

    const char *response =
        "<html><body><h1>WiFi Configured!</h1>"
        "<p>Successfully connected to your WiFi network.</p>"
        "<p>Device will restart in 3 seconds...</p></body></html>";
    httpd_resp_send(req, response, strlen(response));

    return ESP_OK;
}

esp_err_t wifi_provisioning_init(void)
{
    ESP_LOGI(TAG, "WiFi provisioning initialized");
    return ESP_OK;
}

esp_err_t wifi_provisioning_start_ap(void)
{
    ESP_LOGI(TAG, "Starting WiFi AP for provisioning");

    // Stop WiFi first
    esp_wifi_stop();

    // Set WiFi mode to AP
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    // Configure WiFi AP with unique SSID
    const char *ap_ssid = get_setup_ap_ssid();

    wifi_config_t wifi_config = {
        .ap = {.channel = 1, .password = "", .max_connection = 4, .authmode = WIFI_AUTH_OPEN},
    };
    strncpy((char *) wifi_config.ap.ssid, ap_ssid, sizeof(wifi_config.ap.ssid));
    wifi_config.ap.ssid_len = strlen(ap_ssid);

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Wait a bit for netif to be created
    vTaskDelay(pdMS_TO_TICKS(100));

    // Now get the AP network interface
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif == NULL) {
        ESP_LOGE(TAG, "Failed to get AP netif handle");
        return ESP_FAIL;
    }

    // Stop DHCP server first
    esp_netif_dhcps_stop(ap_netif);

    // Set static IP for the AP
    esp_netif_ip_info_t ip_info;
    IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(ap_netif, &ip_info));

    // Start DHCP server
    ESP_ERROR_CHECK(esp_netif_dhcps_start(ap_netif));

    ESP_LOGI(TAG, "WiFi AP started - SSID: %s", ap_ssid);
    ESP_LOGI(TAG, "AP IP address set to 192.168.4.1");

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 16;

    if (httpd_start(&provisioning_server, &config) == ESP_OK) {
        // Serve main webapp - Vue router will handle /provision route
        httpd_uri_t index_uri = {
            .uri = "/", .method = HTTP_GET, .handler = provision_index_handler, .user_ctx = NULL};
        httpd_register_uri_handler(provisioning_server, &index_uri);

        httpd_uri_t provision_uri = {.uri = "/provision",
                                     .method = HTTP_GET,
                                     .handler = provision_index_handler,
                                     .user_ctx = NULL};
        httpd_register_uri_handler(provisioning_server, &provision_uri);

        // Webapp assets
        httpd_uri_t css_uri = {.uri = "/assets/index.css",
                               .method = HTTP_GET,
                               .handler = provision_css_handler,
                               .user_ctx = NULL};
        httpd_register_uri_handler(provisioning_server, &css_uri);

        httpd_uri_t js_uri = {.uri = "/assets/index.js",
                              .method = HTTP_GET,
                              .handler = provision_js_handler,
                              .user_ctx = NULL};
        httpd_register_uri_handler(provisioning_server, &js_uri);

        httpd_uri_t js2_uri = {.uri = "/assets/index2.js",
                               .method = HTTP_GET,
                               .handler = provision_js2_handler,
                               .user_ctx = NULL};
        httpd_register_uri_handler(provisioning_server, &js2_uri);

        httpd_uri_t exif_uri = {.uri = "/assets/exif-reader.js",
                                .method = HTTP_GET,
                                .handler = provision_exif_js_handler,
                                .user_ctx = NULL};
        httpd_register_uri_handler(provisioning_server, &exif_uri);

        httpd_uri_t browser_uri = {.uri = "/assets/browser.js",
                                   .method = HTTP_GET,
                                   .handler = provision_browser_js_handler,
                                   .user_ctx = NULL};
        httpd_register_uri_handler(provisioning_server, &browser_uri);

        httpd_uri_t vite_uri = {.uri = "/assets/__vite-browser-external.js",
                                .method = HTTP_GET,
                                .handler = provision_vite_js_handler,
                                .user_ctx = NULL};
        httpd_register_uri_handler(provisioning_server, &vite_uri);

        httpd_uri_t icon_uri = {.uri = "/icon.svg",
                                .method = HTTP_GET,
                                .handler = provision_icon_handler,
                                .user_ctx = NULL};
        httpd_register_uri_handler(provisioning_server, &icon_uri);

        // Save credentials handler
        httpd_uri_t save_uri = {.uri = "/save",
                                .method = HTTP_POST,
                                .handler = provision_save_handler,
                                .user_ctx = NULL};
        httpd_register_uri_handler(provisioning_server, &save_uri);

        // WiFi scan handler
        httpd_uri_t scan_uri = {.uri = "/api/wifi/scan",
                                .method = HTTP_GET,
                                .handler = provision_scan_handler,
                                .user_ctx = NULL};
        httpd_register_uri_handler(provisioning_server, &scan_uri);

        // Keep-alive handler
        httpd_uri_t keep_alive_uri = {.uri = "/api/keep_alive",
                                      .method = HTTP_GET,
                                      .handler = provision_keep_alive_handler,
                                      .user_ctx = NULL};
        httpd_register_uri_handler(provisioning_server, &keep_alive_uri);

        // iOS captive portal detection
        httpd_uri_t ios_captive = {.uri = "/hotspot-detect.html",
                                   .method = HTTP_GET,
                                   .handler = captive_portal_handler,
                                   .user_ctx = NULL};
        httpd_register_uri_handler(provisioning_server, &ios_captive);

        // Android captive portal detection
        httpd_uri_t android_captive = {.uri = "/generate_204",
                                       .method = HTTP_GET,
                                       .handler = captive_portal_handler,
                                       .user_ctx = NULL};
        httpd_register_uri_handler(provisioning_server, &android_captive);

        // Windows captive portal detection
        httpd_uri_t windows_captive = {.uri = "/connecttest.txt",
                                       .method = HTTP_GET,
                                       .handler = captive_portal_handler,
                                       .user_ctx = NULL};
        httpd_register_uri_handler(provisioning_server, &windows_captive);

        ESP_LOGI(TAG, "Provisioning web server started on http://192.168.4.1");
        ESP_LOGI(TAG, "Captive portal detection enabled for iOS/Android/Windows");

        // Register error handler for 404 (catch-all for unmatched URLs)
        httpd_register_err_handler(provisioning_server, HTTPD_404_NOT_FOUND,
                                   captive_portal_error_handler);
    }

    // Start DNS server for captive portal
    dns_server_start();

    return ESP_OK;
}

esp_err_t wifi_provisioning_stop_ap(void)
{
    dns_server_stop();

    if (provisioning_server) {
        httpd_stop(provisioning_server);
        provisioning_server = NULL;
    }

    esp_wifi_stop();
    ESP_LOGI(TAG, "WiFi AP stopped");

    return ESP_OK;
}

bool wifi_provisioning_is_provisioned(void)
{
    char ssid[WIFI_SSID_MAX_LEN];
    char password[WIFI_PASS_MAX_LEN];

    esp_err_t ret = wifi_manager_load_credentials(ssid, password);
    return (ret == ESP_OK && strlen(ssid) > 0);
}
