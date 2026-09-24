#include "wifi_manager.h"

#include <string.h>

#include "config.h"
#include "config_manager.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "storage.h"
#include "utils.h"

static const char *TAG = "wifi_manager";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

// Reconnect attempts after the first one before giving up.
#define WIFI_MAX_RETRY 5
// Hard cap on one wifi_manager_connect(). Association plus DHCP normally takes
// a few seconds; this only matters when the AP is out of range or DHCP never
// answers, where waiting longer just burns battery (#121).
#define WIFI_CONNECT_TIMEOUT_MS 30000

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;
static bool s_is_connected = false;
// Reconnect policy, shared between wifi_manager_connect()'s caller and the
// event handler (which runs on the event-loop task), so guarded by
// s_policy_lock. A mutex rather than a spinlock: the handler must decide to
// stop retrying and publish WIFI_FAIL_BIT as one step, and event-group calls
// are not allowed inside a critical section.
//  - s_give_up: wifi_manager_stop_connecting() was called; never reconnect.
//  - s_keep_trying: wifi_manager_keep_reconnecting() was called; reconnect
//    without the WIFI_MAX_RETRY limit.
//  - s_retries_exhausted: the handler stopped reconnecting and set
//    WIFI_FAIL_BIT, so nothing is in progress any more.
static SemaphoreHandle_t s_policy_lock = NULL;
static bool s_give_up = false;
static bool s_keep_trying = false;
static bool s_retries_exhausted = false;
// Consecutive attempts the AP rejected in a way that points at the password
// (see is_auth_rejection()). Reset whenever the AP accepts us or an attempt
// fails for some other reason, so only an unbroken run of rejections counts.
static int s_auth_rejects = 0;  // guarded by s_policy_lock too

// Disconnect reasons that mean the AP turned the credentials down, as opposed
// to it being absent or out of range (NO_AP_FOUND, BEACON_TIMEOUT, ...). A
// wrong WPA2 passphrase surfaces as a 4-way handshake timeout.
static bool is_auth_rejection(uint8_t reason)
{
    return reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT || reason == WIFI_REASON_AUTH_FAIL ||
           reason == WIFI_REASON_HANDSHAKE_TIMEOUT;
}
static esp_netif_t *s_sta_netif = NULL;

static void apply_dns_override(void);

static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                          void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        // Bring up an IPv6 link-local address so mDNS can answer AAAA queries.
        // Without one the responder stays silent on AAAA, and clients resolving
        // <name>.local wait out their full resolver timeout (~5s per request)
        // before falling back to the A record.
        esp_netif_create_ip6_linklocal(s_sta_netif);
        xSemaphoreTake(s_policy_lock, portMAX_DELAY);
        s_auth_rejects = 0;  // the AP accepted the credentials
        xSemaphoreGive(s_policy_lock);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *disc =
            (const wifi_event_sta_disconnected_t *) event_data;
        xSemaphoreTake(s_policy_lock, portMAX_DELAY);
        if (disc && is_auth_rejection(disc->reason)) {
            s_auth_rejects++;
        } else {
            s_auth_rejects = 0;
        }
        // Background reconnects (s_keep_trying) wait out an absent AP or a
        // silent DHCP server indefinitely, but not an AP that keeps refusing
        // the password: that gets the same number of attempts as a normal
        // connect, then WIFI_FAIL_BIT so the owner of the retry can react.
        bool retry = !s_give_up && (s_keep_trying ? s_auth_rejects <= WIFI_MAX_RETRY
                                                  : s_retry_num < WIFI_MAX_RETRY);
        if (retry) {
            s_retry_num++;
            esp_wifi_connect();
        } else {
            s_retries_exhausted = true;
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        xSemaphoreGive(s_policy_lock);
        ESP_LOGI(TAG, "%s", retry ? "retry to connect to the AP" : "giving up on the AP");
        s_is_connected = false;
        // Waiters on WIFI_CONNECTED_BIT (e.g. main.c's late_wifi_task) must
        // see the link as down again, not a stale bit from an earlier IP.
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        // Applied after the address is up so it overrides DHCP-provided DNS
        // servers too (#43).
        apply_dns_override();
        s_retry_num = 0;
        s_is_connected = true;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_GOT_IP6) {
        ip_event_got_ip6_t *event = (ip_event_got_ip6_t *) event_data;
        ESP_LOGI(TAG, "got ip6:" IPV6STR, IPV62STR(event->ip6_info.ip));
    }
}

esp_err_t wifi_manager_set_performance_mode(bool enable)
{
    // Modem power save adds ~100ms+ of latency to every round trip, which
    // throttles the web UI hard: bulk transfer speed is roughly one TCP send
    // buffer (~5.7KB) per round trip, i.e. ~45KB/s at 130ms RTT. Full RX
    // (WIFI_PS_NONE) costs ~60-70mA extra while the radio is up, so it is only
    // enabled when someone may actually be using the UI — the policy lives in
    // power_manager's sleep_timer_task.
    static bool applied = false;
    static bool current = false;
    if (applied && current == enable) {
        return ESP_OK;
    }

    esp_err_t err = esp_wifi_set_ps(enable ? WIFI_PS_NONE : WIFI_PS_MIN_MODEM);
    if (err == ESP_OK) {
        applied = true;
        current = enable;
        ESP_LOGI(TAG, "WiFi power save %s (%s mode)", enable ? "disabled" : "enabled",
                 enable ? "performance" : "power-save");
    }
    return err;
}

esp_err_t wifi_manager_update_hostname(void)
{
    if (!s_sta_netif) {
        return ESP_ERR_INVALID_STATE;
    }

    // DHCP hostname from the device name (CamelCase, shown in router device
    // lists). The router picks it up at the next DHCP negotiation (reconnect).
    char hostname[64];
    sanitize_dhcp_hostname(config_manager_get_device_name(), hostname, sizeof(hostname));
    esp_err_t err = esp_netif_set_hostname(s_sta_netif, hostname);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set DHCP hostname: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "DHCP hostname set to: %s", hostname);
    return ESP_OK;
}

esp_err_t wifi_manager_init(void)
{
    s_wifi_event_group = xEventGroupCreate();
    s_policy_lock = xSemaphoreCreateMutex();
    if (!s_wifi_event_group || !s_policy_lock) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Create both STA and AP network interfaces
    s_sta_netif = esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_manager_update_hostname();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    esp_event_handler_instance_t instance_got_ip6;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &event_handler, NULL, &instance_got_ip));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_GOT_IP6, &event_handler,
                                                        NULL, &instance_got_ip6));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    // Don't start WiFi here - let wifi_manager_connect() or wifi_provisioning_start_ap() start it
    // ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_manager_init finished.");

    return ESP_OK;
}

esp_err_t wifi_manager_apply_ip_config(void)
{
    if (!s_sta_netif) {
        return ESP_ERR_INVALID_STATE;
    }

    if (config_manager_get_ip_mode() == IP_MODE_STATIC) {
        esp_netif_ip_info_t ip_info = {0};
        if (esp_netif_str_to_ip4(config_manager_get_static_ip(), &ip_info.ip) != ESP_OK ||
            esp_netif_str_to_ip4(config_manager_get_static_netmask(), &ip_info.netmask) != ESP_OK ||
            esp_netif_str_to_ip4(config_manager_get_static_gateway(), &ip_info.gw) != ESP_OK) {
            // Never brick the connection on a malformed config — fall back to
            // DHCP so the frame stays reachable and the user can fix it.
            ESP_LOGE(TAG, "Invalid static IP config, falling back to DHCP");
            esp_netif_dhcpc_start(s_sta_netif);
            return ESP_ERR_INVALID_ARG;
        }

        esp_netif_dhcpc_stop(s_sta_netif);
        esp_netif_set_ip_info(s_sta_netif, &ip_info);
        ESP_LOGI(TAG, "Static IP applied: %s/%s gw %s", config_manager_get_static_ip(),
                 config_manager_get_static_netmask(), config_manager_get_static_gateway());
    } else {
        // Make sure DHCP runs when switching back from a static config.
        // Returns ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED in the normal case.
        esp_netif_dhcpc_start(s_sta_netif);
    }
    return ESP_OK;
}

// Apply the DNS override (if configured). Called after GOT_IP so it takes
// precedence over DHCP-provided servers in DHCP mode; in static mode it is the
// only DNS source (defaults to the gateway when unset).
static void apply_dns_override(void)
{
    const char *dns = config_manager_get_dns_server();
    if ((dns == NULL || dns[0] == '\0') && config_manager_get_ip_mode() == IP_MODE_STATIC) {
        dns = config_manager_get_static_gateway();
    }
    if (dns == NULL || dns[0] == '\0') {
        return;
    }

    esp_netif_dns_info_t dns_info = {0};
    if (esp_netif_str_to_ip4(dns, &dns_info.ip.u_addr.ip4) != ESP_OK) {
        ESP_LOGE(TAG, "Invalid DNS server: %s", dns);
        return;
    }
    dns_info.ip.type = ESP_IPADDR_TYPE_V4;
    esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dns_info);
    ESP_LOGI(TAG, "DNS server set to: %s", dns);
}

esp_err_t wifi_manager_connect(const char *ssid, const char *password)
{
    if (!ssid || strlen(ssid) == 0) {
        ESP_LOGE(TAG, "SSID is empty");
        return ESP_ERR_INVALID_ARG;
    }

    wifi_manager_apply_ip_config();

    wifi_config_t wifi_config = {0};
    strncpy((char *) wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    if (password) {
        strncpy((char *) wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    }
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    // Stop WiFi if it's running, then set config
    esp_wifi_stop();

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));  // Enable power save at boot/connect

    xSemaphoreTake(s_policy_lock, portMAX_DELAY);
    s_retry_num = 0;
    s_give_up = false;
    s_keep_trying = false;
    s_retries_exhausted = false;
    s_auth_rejects = 0;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    xSemaphoreGive(s_policy_lock);
    EventBits_t bits =
        xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE,
                            pdFALSE, pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));

    if (bits & WIFI_CONNECTED_BIT) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            ESP_LOGI(TAG, "connected to ap SSID:%s (RSSI %d dBm, channel %d)", ssid, ap.rssi,
                     ap.primary);
        } else {
            ESP_LOGI(TAG, "connected to ap SSID:%s", ssid);
        }
        return ESP_OK;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGW(TAG, "Failed to connect to SSID:%s after %d attempts", ssid, WIFI_MAX_RETRY + 1);
        return ESP_FAIL;
    }

    xSemaphoreTake(s_policy_lock, portMAX_DELAY);
    bool rejected = s_auth_rejects > 0;
    xSemaphoreGive(s_policy_lock);
    if (rejected) {
        // Out of time while the AP was still rejecting the password: that is
        // a credential failure that merely ran slow (a wrong passphrase costs
        // a multi-second handshake timeout per attempt), not a slow network.
        // Report it as ESP_FAIL so an interactive boot still falls back to
        // provisioning rather than retrying a wrong password forever.
        ESP_LOGW(TAG, "SSID:%s rejected the credentials; giving up after %d ms", ssid,
                 WIFI_CONNECT_TIMEOUT_MS);
        wifi_manager_stop_connecting();
        return ESP_FAIL;
    }

    // Timed out otherwise: associated but no IP yet (DHCP not answering), or
    // the AP not answering at all (still booting, out of range). Neither says
    // the credentials are wrong, so this is not ESP_FAIL. The attempt is still
    // running; the caller decides whether to stop it or keep it going.
    ESP_LOGW(TAG, "Timed out connecting to SSID:%s after %d ms", ssid, WIFI_CONNECT_TIMEOUT_MS);
    return ESP_ERR_TIMEOUT;
}

void wifi_manager_stop_connecting(void)
{
    // Stop the event handler from reconnecting on its own and power the radio
    // down, so it isn't left churning while the caller moves on (#121).
    // esp_wifi_stop() rather than a disconnect: the handler may already have
    // decided to retry and be about to call esp_wifi_connect(), which a
    // disconnect issued first would not cancel; once WiFi is stopped that
    // call simply fails.
    xSemaphoreTake(s_policy_lock, portMAX_DELAY);
    s_give_up = true;
    s_keep_trying = false;
    xSemaphoreGive(s_policy_lock);
    esp_wifi_stop();
    s_is_connected = false;
}

void wifi_manager_keep_reconnecting(void)
{
    // Lift the retry limit. If the handler already ran out of retries in the
    // meantime nothing is in progress, so start a fresh attempt ourselves.
    // Under the lock, so the handler can't publish WIFI_FAIL_BIT for the old
    // attempt after we have cleared it and started a new one.
    xSemaphoreTake(s_policy_lock, portMAX_DELAY);
    s_give_up = false;
    s_keep_trying = true;
    if (s_retries_exhausted) {
        s_retries_exhausted = false;
        xEventGroupClearBits(s_wifi_event_group, WIFI_FAIL_BIT);
        esp_wifi_connect();
    }
    xSemaphoreGive(s_policy_lock);
}

esp_err_t wifi_manager_disconnect(void)
{
    s_is_connected = false;
    return esp_wifi_disconnect();
}

bool wifi_manager_is_connected(void)
{
    return s_is_connected;
}

esp_err_t wifi_manager_get_ip(char *ip_str, size_t len)
{
    if (!ip_str || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) {
        return ESP_FAIL;
    }

    esp_netif_ip_info_t ip_info;
    esp_err_t ret = esp_netif_get_ip_info(netif, &ip_info);
    if (ret != ESP_OK) {
        return ret;
    }

    snprintf(ip_str, len, IPSTR, IP2STR(&ip_info.ip));
    return ESP_OK;
}

esp_err_t wifi_manager_save_credentials(const char *ssid, const char *password)
{
    nvs_handle_t nvs_handle;
    esp_err_t err;

    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(nvs_handle, NVS_WIFI_SSID_KEY, ssid);
    if (err != ESP_OK) {
        nvs_close(nvs_handle);
        return err;
    }

    err = nvs_set_str(nvs_handle, NVS_WIFI_PASS_KEY, password);
    if (err != ESP_OK) {
        nvs_close(nvs_handle);
        return err;
    }

    err = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    return err;
}

esp_err_t wifi_manager_load_credentials(char *ssid, char *password)
{
    nvs_handle_t nvs_handle;
    esp_err_t err;

    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t ssid_len = WIFI_SSID_MAX_LEN;
    err = nvs_get_str(nvs_handle, NVS_WIFI_SSID_KEY, ssid, &ssid_len);
    if (err != ESP_OK) {
        nvs_close(nvs_handle);
        return err;
    }

    size_t pass_len = WIFI_PASS_MAX_LEN;
    err = nvs_get_str(nvs_handle, NVS_WIFI_PASS_KEY, password, &pass_len);
    nvs_close(nvs_handle);

    return err;
}

EventGroupHandle_t wifi_manager_get_event_group(void)
{
    return s_wifi_event_group;
}

int wifi_manager_scan(wifi_ap_record_t *results, int max_results)
{
    if (!results || max_results <= 0) {
        return 0;
    }

    // Save current WiFi mode
    wifi_mode_t original_mode;
    esp_err_t err = esp_wifi_get_mode(&original_mode);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get WiFi mode: %s", esp_err_to_name(err));
        return 0;
    }

    // Switch to APSTA mode if currently in AP-only mode
    if (original_mode == WIFI_MODE_AP) {
        err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set APSTA mode: %s", esp_err_to_name(err));
            return 0;
        }
    }

    // Start blocking scan on all channels
    wifi_scan_config_t scan_config = {0};
    err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi scan failed: %s", esp_err_to_name(err));
        if (original_mode == WIFI_MODE_AP) {
            esp_wifi_set_mode(original_mode);
        }
        return 0;
    }

    // Get number of APs found
    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);

    if (ap_count == 0) {
        ESP_LOGI(TAG, "No APs found");
        if (original_mode == WIFI_MODE_AP) {
            esp_wifi_set_mode(original_mode);
        }
        return 0;
    }

    // Limit to max_results
    uint16_t fetch_count = (ap_count > (uint16_t) max_results) ? (uint16_t) max_results : ap_count;
    err = esp_wifi_scan_get_ap_records(&fetch_count, results);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get scan results: %s", esp_err_to_name(err));
        if (original_mode == WIFI_MODE_AP) {
            esp_wifi_set_mode(original_mode);
        }
        return 0;
    }

    // Restore original WiFi mode
    if (original_mode == WIFI_MODE_AP) {
        esp_wifi_set_mode(original_mode);
    }

    ESP_LOGI(TAG, "WiFi scan found %d APs (returning %d)", ap_count, fetch_count);
    return (int) fetch_count;
}
