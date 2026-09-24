#include <gtest/gtest.h>

#include <functional>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
#include "config_manager.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "rotation_network.h"
#include "wifi_manager.h"
}

static int64_t now_us;
static esp_err_t stop_error;
static bool ha_configured, ota_finished;
static int post_wait, ota_waits;
static std::vector<std::string> calls;
static std::function<void()> pending_event;
static std::mutex lifecycle_mutex;
static void (*wifi_handler)(void *, esp_event_base_t, int32_t, void *);
static std::function<void()> on_wait, on_start;
static EventBits_t event_bits;
static TickType_t waited_ticks;
static int connects, disconnects, stops;
static wifi_mode_t wifi_mode;
static esp_err_t start_error;
static esp_netif_t netif;
extern "C" {
const char *esp_err_to_name(esp_err_t)
{
    return "test error";
}
int64_t esp_timer_get_time(void)
{
    return now_us;
}
SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    return &lifecycle_mutex;
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t handle, TickType_t)
{
    static_cast<std::mutex *>(handle)->lock();
    return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t handle)
{
    static_cast<std::mutex *>(handle)->unlock();
    if (pending_event) {
        auto event = pending_event;
        pending_event = nullptr;
        event();
    }
    return pdTRUE;
}
bool ha_is_configured(void)
{
    return ha_configured;
}
int utils_get_post_rotate_wait_sec(void)
{
    return post_wait;
}
bool ota_wait_for_check(int timeout_ms)
{
    EXPECT_EQ(timeout_ms, 30000);
    ota_waits++;
    calls.push_back("ota_complete");
    return ota_finished;
}
void esp_sntp_stop(void)
{
    calls.push_back("sntp_stop");
}
EventGroupHandle_t xEventGroupCreate(void)
{
    event_bits = 0;
    return &event_bits;
}
EventBits_t xEventGroupSetBits(EventGroupHandle_t, EventBits_t bits)
{
    return event_bits |= bits;
}
EventBits_t xEventGroupClearBits(EventGroupHandle_t, EventBits_t bits)
{
    return event_bits &= ~bits;
}
EventBits_t xEventGroupWaitBits(EventGroupHandle_t, EventBits_t bits, BaseType_t clear, BaseType_t,
                                TickType_t ticks)
{
    waited_ticks = ticks;
    if (on_wait)
        on_wait();
    auto result = event_bits & bits;
    if (!result)
        now_us += ticks * 1000LL;
    if (clear)
        event_bits &= ~bits;
    return result;
}
esp_err_t esp_event_loop_create_default(void)
{
    return ESP_OK;
}
esp_err_t esp_event_handler_instance_register(esp_event_base_t base, int32_t,
                                              void (*handler)(void *, esp_event_base_t, int32_t,
                                                              void *),
                                              void *, esp_event_handler_instance_t *)
{
    if (base == WIFI_EVENT)
        wifi_handler = handler;
    return ESP_OK;
}
esp_err_t esp_wifi_init(const wifi_init_config_t *)
{
    return ESP_OK;
}
esp_err_t esp_wifi_set_mode(wifi_mode_t mode)
{
    wifi_mode = mode;
    return ESP_OK;
}
esp_err_t esp_wifi_get_mode(wifi_mode_t *mode)
{
    *mode = wifi_mode;
    return ESP_OK;
}
esp_err_t esp_wifi_set_config(int, const wifi_config_t *)
{
    return ESP_OK;
}
esp_err_t esp_wifi_start(void)
{
    if (start_error != ESP_OK)
        return start_error;
    pending_event = [] {
        wifi_handler(nullptr, WIFI_EVENT, WIFI_EVENT_STA_START, nullptr);
        if (on_start)
            on_start();
    };
    return ESP_OK;
}
esp_err_t esp_wifi_stop(void)
{
    stops++;
    calls.push_back("wifi_stop");
    pending_event = [] { wifi_handler(nullptr, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, nullptr); };
    return stop_error;
}
esp_err_t esp_wifi_connect(void)
{
    connects++;
    return ESP_OK;
}
esp_err_t esp_wifi_disconnect(void)
{
    disconnects++;
    pending_event = [] { wifi_handler(nullptr, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, nullptr); };
    return ESP_OK;
}
esp_err_t esp_wifi_set_ps(int)
{
    return ESP_OK;
}
esp_err_t esp_netif_init(void)
{
    return ESP_OK;
}
esp_netif_t *esp_netif_create_default_wifi_sta(void)
{
    return &netif;
}
esp_netif_t *esp_netif_create_default_wifi_ap(void)
{
    return &netif;
}
esp_err_t esp_netif_create_ip6_linklocal(esp_netif_t *)
{
    return ESP_OK;
}
esp_err_t esp_netif_set_hostname(esp_netif_t *, const char *)
{
    return ESP_OK;
}
esp_err_t esp_netif_str_to_ip4(const char *, esp_ip4_addr_t *)
{
    return ESP_OK;
}
esp_err_t esp_netif_dhcpc_start(esp_netif_t *)
{
    return ESP_OK;
}
esp_err_t esp_netif_dhcpc_stop(esp_netif_t *)
{
    return ESP_OK;
}
esp_err_t esp_netif_set_ip_info(esp_netif_t *, const esp_netif_ip_info_t *)
{
    return ESP_OK;
}
esp_err_t esp_netif_set_dns_info(esp_netif_t *, int, const esp_netif_dns_info_t *)
{
    return ESP_OK;
}
const char *config_manager_get_device_name(void)
{
    return "test";
}
ip_mode_t config_manager_get_ip_mode(void)
{
    return IP_MODE_DHCP;
}
const char *config_manager_get_static_ip(void)
{
    return "";
}
const char *config_manager_get_static_netmask(void)
{
    return "";
}
const char *config_manager_get_static_gateway(void)
{
    return "";
}
const char *config_manager_get_dns_server(void)
{
    return "";
}
void sanitize_dhcp_hostname(const char *, char *out, size_t size)
{
    snprintf(out, size, "test");
}
}

static void got_ip()
{
    ip_event_got_ip_t event = {};
    wifi_handler(nullptr, IP_EVENT, IP_EVENT_STA_GOT_IP, &event);
}
static void disconnected(int reason)
{
    wifi_event_sta_disconnected_t event = {reason};
    wifi_handler(nullptr, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &event);
}
class WifiLifecycle : public testing::Test
{
   protected:
    void SetUp() override
    {
        on_wait = on_start = pending_event = nullptr;
        start_error = stop_error = ESP_OK;
        connects = disconnects = stops = ota_waits = 0;
        ha_configured = false;
        ota_finished = true;
        post_wait = 0;
        calls.clear();
        ASSERT_EQ(wifi_manager_init(), ESP_OK);
    }
    void Connect()
    {
        on_start = got_ip;
        ASSERT_EQ(wifi_manager_connect("test", "password"), ESP_OK);
        ASSERT_TRUE(wifi_manager_is_connected());
        calls.clear();
        stops = 0;
    }
};
TEST(WifiBeforeInit, StopIsSafe)
{
    EXPECT_EQ(wifi_manager_stop(), ESP_OK);
}
TEST(WifiBeforeInit, LocalWakeDoesNotTouchNetworkStack)
{
    rotation_network_done();
    EXPECT_TRUE(calls.empty());
}
TEST_F(WifiLifecycle, StopSuppressesQueuedDisconnectStartAndIp)
{
    Connect();
    EXPECT_EQ(wifi_manager_stop(), ESP_OK);
    disconnected(0);
    wifi_handler(nullptr, WIFI_EVENT, WIFI_EVENT_STA_START, nullptr);
    got_ip();
    EXPECT_EQ(connects, 1);
    EXPECT_FALSE(wifi_manager_is_connected());
    EXPECT_EQ(event_bits & WIFI_CONNECTED_BIT, 0U);
}
TEST_F(WifiLifecycle, DisconnectPreservesRadioAndSuppressesRetry)
{
    Connect();
    EXPECT_EQ(wifi_manager_disconnect(), ESP_OK);
    disconnected(0);
    EXPECT_EQ(stops, 0);
    EXPECT_EQ(connects, 1);
    EXPECT_FALSE(wifi_manager_is_connected());
}
TEST_F(WifiLifecycle, ExplicitConnectAfterStopRestoresRetries)
{
    Connect();
    ASSERT_EQ(wifi_manager_stop(), ESP_OK);
    Connect();
    disconnected(0);
    EXPECT_EQ(connects, 3);
    EXPECT_FALSE(wifi_manager_is_connected());
    EXPECT_EQ(event_bits & WIFI_CONNECTED_BIT, 0U);
    got_ip();
    EXPECT_TRUE(wifi_manager_is_connected());
}
TEST_F(WifiLifecycle, FastIpResultSurvivesStart)
{
    Connect();
}
TEST_F(WifiLifecycle, RepeatedStopNormalizesAlreadyStopped)
{
    Connect();
    EXPECT_EQ(wifi_manager_stop(), ESP_OK);
    stop_error = ESP_ERR_WIFI_NOT_STARTED;
    EXPECT_EQ(wifi_manager_stop(), ESP_OK);
    EXPECT_EQ(connects, 1);
}
TEST_F(WifiLifecycle, DriverStopFailureIsReportedWithoutRetry)
{
    Connect();
    stop_error = ESP_FAIL;
    EXPECT_EQ(wifi_manager_stop(), ESP_FAIL);
    disconnected(0);
    EXPECT_EQ(connects, 1);
}
TEST_F(WifiLifecycle, FailedStartDoesNotWaitOrAcceptLateIp)
{
    start_error = ESP_FAIL;
    bool waited = false;
    on_wait = [&] { waited = true; };
    EXPECT_EQ(wifi_manager_connect("test", "password"), ESP_FAIL);
    got_ip();
    EXPECT_FALSE(waited);
    EXPECT_FALSE(wifi_manager_is_connected());
}
TEST_F(WifiLifecycle, ProvisioningCanConnectDirectlyAfterInit)
{
    wifi_handler(nullptr, WIFI_EVENT, WIFI_EVENT_STA_START, nullptr);
    got_ip();
    EXPECT_TRUE(wifi_manager_is_connected());
    EXPECT_EQ(connects, 1);
}
TEST_F(WifiLifecycle, CompletedNetworkWorkStopsSntpThenWifi)
{
    Connect();
    rotation_network_done();
    EXPECT_EQ(calls, (std::vector<std::string>{"ota_complete", "sntp_stop", "wifi_stop"}));
    EXPECT_FALSE(wifi_manager_is_connected());
}
TEST_F(WifiLifecycle, HomeAssistantKeepsConnection)
{
    Connect();
    ha_configured = true;
    rotation_network_done();
    EXPECT_EQ(stops, 0);
    EXPECT_EQ(ota_waits, 0);
    EXPECT_TRUE(wifi_manager_is_connected());
}
TEST_F(WifiLifecycle, ServerWindowKeepsConnection)
{
    Connect();
    post_wait = 10;
    rotation_network_done();
    EXPECT_EQ(stops, 0);
    EXPECT_EQ(ota_waits, 0);
    EXPECT_TRUE(wifi_manager_is_connected());
}
TEST_F(WifiLifecycle, ActiveOtaWorkerKeepsConnectionAfterWait)
{
    Connect();
    ota_finished = false;
    rotation_network_done();
    EXPECT_EQ(stops, 0);
    EXPECT_EQ(ota_waits, 1);
    EXPECT_TRUE(wifi_manager_is_connected());
    EXPECT_EQ(calls, (std::vector<std::string>{"ota_complete"}));
}

TEST_F(WifiLifecycle, FailedConnectionStillStopsRadioBeforeFallback)
{
    on_wait = [] {
        for (int i = 0; i < 6; ++i)
            disconnected(0);
    };
    ASSERT_EQ(wifi_manager_connect("missing", "password"), ESP_FAIL);
    calls.clear();
    rotation_network_done();
    EXPECT_EQ(calls, (std::vector<std::string>{"ota_complete", "sntp_stop", "wifi_stop"}));
    int before = connects;
    disconnected(0);
    EXPECT_EQ(connects, before);
}
TEST_F(WifiLifecycle, FailedStopPreventsStartingAnotherConnection)
{
    Connect();
    stop_error = ESP_FAIL;
    EXPECT_EQ(wifi_manager_connect("other", "password"), ESP_FAIL);
    EXPECT_EQ(connects, 1);
}
