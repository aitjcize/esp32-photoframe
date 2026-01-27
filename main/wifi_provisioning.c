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
#include "wifi_manager.h"

static const char *TAG = "wifi_prov";
static httpd_handle_t provisioning_server = NULL;

// Structure to pass credentials to test task
typedef struct {
    char ssid[WIFI_SSID_MAX_LEN];
    char password[WIFI_PASS_MAX_LEN];
} wifi_test_params_t;

// Webapp assets - same as http_server.c
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");
extern const uint8_t index_css_start[] asm("_binary_index_css_start");
extern const uint8_t index_css_end[] asm("_binary_index_css_end");
extern const uint8_t index_js_start[] asm("_binary_index_js_start");
extern const uint8_t index_js_end[] asm("_binary_index_js_end");
extern const uint8_t index2_js_start[] asm("_binary_index2_js_start");
extern const uint8_t index2_js_end[] asm("_binary_index2_js_end");
extern const uint8_t exif_reader_js_start[] asm("_binary_exif_reader_js_start");
extern const uint8_t exif_reader_js_end[] asm("_binary_exif_reader_js_end");
extern const uint8_t browser_js_start[] asm("_binary_browser_js_start");
extern const uint8_t browser_js_end[] asm("_binary_browser_js_end");
extern const uint8_t vite_browser_external_js_start[] asm(
    "_binary___vite_browser_external_js_start");
extern const uint8_t vite_browser_external_js_end[] asm("_binary___vite_browser_external_js_end");
extern const uint8_t favicon_svg_start[] asm("_binary_favicon_svg_start");
extern const uint8_t favicon_svg_end[] asm("_binary_favicon_svg_end");

static esp_err_t provision_index_handler(httpd_req_t *req)
{
    power_manager_reset_sleep_timer();

    // If accessing root, redirect to /provision
    if (strcmp(req->uri, "/") == 0) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/provision");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    const size_t index_html_size = (index_html_end - index_html_start);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, (const char *) index_html_start, index_html_size);
    return ESP_OK;
}

static esp_err_t provision_css_handler(httpd_req_t *req)
{
    power_manager_reset_sleep_timer();
    const size_t index_css_size = (index_css_end - index_css_start);
    httpd_resp_set_type(req, "text/css");
    httpd_resp_send(req, (const char *) index_css_start, index_css_size);
    return ESP_OK;
}

static esp_err_t provision_js_handler(httpd_req_t *req)
{
    power_manager_reset_sleep_timer();
    const size_t index_js_size = (index_js_end - index_js_start);
    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_send(req, (const char *) index_js_start, index_js_size);
    return ESP_OK;
}

static esp_err_t provision_js2_handler(httpd_req_t *req)
{
    power_manager_reset_sleep_timer();
    const size_t index2_js_size = (index2_js_end - index2_js_start);
    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_send(req, (const char *) index2_js_start, index2_js_size);
    return ESP_OK;
}

static esp_err_t provision_exif_js_handler(httpd_req_t *req)
{
    power_manager_reset_sleep_timer();
    const size_t exif_reader_js_size = (exif_reader_js_end - exif_reader_js_start);
    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_send(req, (const char *) exif_reader_js_start, exif_reader_js_size);
    return ESP_OK;
}

static esp_err_t provision_browser_js_handler(httpd_req_t *req)
{
    power_manager_reset_sleep_timer();
    const size_t browser_js_size = (browser_js_end - browser_js_start);
    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_send(req, (const char *) browser_js_start, browser_js_size);
    return ESP_OK;
}

static esp_err_t provision_vite_js_handler(httpd_req_t *req)
{
    power_manager_reset_sleep_timer();
    const size_t vite_js_size = (vite_browser_external_js_end - vite_browser_external_js_start);
    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_send(req, (const char *) vite_browser_external_js_start, vite_js_size);
    return ESP_OK;
}

static esp_err_t provision_favicon_handler(httpd_req_t *req)
{
    power_manager_reset_sleep_timer();
    const size_t favicon_svg_size = (favicon_svg_end - favicon_svg_start);
    httpd_resp_set_type(req, "image/svg+xml");
    httpd_resp_send(req, (const char *) favicon_svg_start, favicon_svg_size);
    return ESP_OK;
}

// Handler for captive portal detection URLs
static esp_err_t captive_portal_handler(httpd_req_t *req)
{
    power_manager_reset_sleep_timer();

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
    power_manager_reset_sleep_timer();

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

static esp_err_t provision_save_handler(httpd_req_t *req)
{
    power_manager_reset_sleep_timer();

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

    char *ssid_start = strstr(buf, "ssid=");
    char *pass_start = strstr(buf, "&password=");
    char *name_start = strstr(buf, "&deviceName=");

    if (!ssid_start) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing SSID");
        return ESP_FAIL;
    }

    ssid_start += 5;
    char *ssid_end = pass_start ? pass_start : (buf + ret);
    int ssid_len = ssid_end - ssid_start;
    if (ssid_len > 0 && ssid_len < WIFI_SSID_MAX_LEN) {
        strncpy(ssid, ssid_start, ssid_len);
        ssid[ssid_len] = '\0';

        for (int i = 0; i < ssid_len; i++) {
            if (ssid[i] == '+')
                ssid[i] = ' ';
            if (ssid[i] == '%' && i + 2 < ssid_len) {
                char hex[3] = {ssid[i + 1], ssid[i + 2], 0};
                ssid[i] = (char) strtol(hex, NULL, 16);
                memmove(&ssid[i + 1], &ssid[i + 3], ssid_len - i - 2);
                ssid_len -= 2;
            }
        }
    }

    if (pass_start) {
        pass_start += 10;
        char *pass_end = name_start ? name_start : (buf + ret);
        int pass_len = pass_end - pass_start;
        if (pass_len > 0 && pass_len < WIFI_PASS_MAX_LEN) {
            strncpy(password, pass_start, pass_len);
            password[pass_len] = '\0';

            for (int i = 0; i < pass_len; i++) {
                if (password[i] == '+')
                    password[i] = ' ';
                if (password[i] == '%' && i + 2 < pass_len) {
                    char hex[3] = {password[i + 1], password[i + 2], 0};
                    password[i] = (char) strtol(hex, NULL, 16);
                    memmove(&password[i + 1], &password[i + 3], pass_len - i - 2);
                    pass_len -= 2;
                }
            }
        }
    }

    // Parse device name (optional)
    if (name_start) {
        name_start += 12;  // Skip "&deviceName="
        int name_len = (buf + ret) - name_start;
        if (name_len > 0 && name_len < DEVICE_NAME_MAX_LEN) {
            strncpy(device_name, name_start, name_len);
            device_name[name_len] = '\0';

            // URL decode device name
            for (int i = 0; i < name_len; i++) {
                if (device_name[i] == '+')
                    device_name[i] = ' ';
                if (device_name[i] == '%' && i + 2 < name_len) {
                    char hex[3] = {device_name[i + 1], device_name[i + 2], 0};
                    device_name[i] = (char) strtol(hex, NULL, 16);
                    memmove(&device_name[i + 1], &device_name[i + 3], name_len - i - 2);
                    name_len -= 2;
                }
            }
        }
    }

    // Use default if device name is empty
    if (strlen(device_name) == 0) {
        strncpy(device_name, DEFAULT_DEVICE_NAME, DEVICE_NAME_MAX_LEN - 1);
        device_name[DEVICE_NAME_MAX_LEN - 1] = '\0';
    }

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

    // Configure WiFi AP
    wifi_config_t wifi_config = {
        .ap = {.ssid = "PhotoFrame-Setup",
               .ssid_len = strlen("PhotoFrame-Setup"),
               .channel = 1,
               .password = "",
               .max_connection = 4,
               .authmode = WIFI_AUTH_OPEN},
    };

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

    ESP_LOGI(TAG, "WiFi AP started - SSID: PhotoFrame-Setup");
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

        httpd_uri_t favicon_uri = {.uri = "/favicon.svg",
                                   .method = HTTP_GET,
                                   .handler = provision_favicon_handler,
                                   .user_ctx = NULL};
        httpd_register_uri_handler(provisioning_server, &favicon_uri);

        // Save credentials handler
        httpd_uri_t save_uri = {.uri = "/save",
                                .method = HTTP_POST,
                                .handler = provision_save_handler,
                                .user_ctx = NULL};
        httpd_register_uri_handler(provisioning_server, &save_uri);

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
