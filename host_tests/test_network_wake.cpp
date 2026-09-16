#include <gtest/gtest.h>

#include <functional>

extern "C" {
#include "config_manager.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "network_policy.h"
#include "network_wake.h"
#include "wifi_manager.h"
}

static int64_t now_us;
static esp_reset_reason_t reset_reason;
static bool allocation_ok;
static void (*deadline_task)(void *);
static void (*wifi_handler)(void *, esp_event_base_t, int32_t, void *);
static std::function<void()> on_wait, on_start;
static EventBits_t event_bits;
static TickType_t waited_ticks;
static int connects, disconnects, stops;
static wifi_mode_t wifi_mode;
static esp_err_t start_error;
static esp_netif_t netif;
struct Restart {
};

extern "C" {
const char *esp_err_to_name(esp_err_t)
{
    return "test error";
}
int64_t esp_timer_get_time(void)
{
    return now_us;
}
esp_reset_reason_t esp_reset_reason(void)
{
    return reset_reason;
}
void esp_restart(void)
{
    throw Restart();
}
void vTaskDelay(TickType_t ticks)
{
    now_us += ticks * 1000LL;
}
BaseType_t xTaskCreate(void (*task)(void *), const char *, uint32_t, void *, unsigned, void *)
{
    deadline_task = task;
    return allocation_ok ? pdPASS : 0;
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
    wifi_handler(nullptr, WIFI_EVENT, WIFI_EVENT_STA_START, nullptr);
    if (on_start)
        on_start();
    return ESP_OK;
}
esp_err_t esp_wifi_stop(void)
{
    stops++;
    return ESP_OK;
}
esp_err_t esp_wifi_connect(void)
{
    connects++;
    return ESP_OK;
}
esp_err_t esp_wifi_disconnect(void)
{
    disconnects++;
    wifi_event_sta_disconnected_t event = {0};
    wifi_handler(nullptr, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &event);
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
class NetworkWake : public testing::Test
{
   protected:
    void SetUp() override
    {
        now_us = 1000000;
        reset_reason = ESP_RST_POWERON;
        allocation_ok = true;
        on_wait = on_start = nullptr;
        connects = disconnects = stops = 0;
        start_error = ESP_OK;
        ASSERT_FALSE(network_wake_init());
        ASSERT_EQ(wifi_manager_init(), ESP_OK);
        wifi_manager_disconnect();
        disconnects = 0;
    }
};
TEST_F(NetworkWake, MissingEventsTimesOutIncludingDhcp)
{
    on_wait = [] { wifi_handler(nullptr, WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, nullptr); };
    EXPECT_EQ(wifi_manager_connect("test", "password", now_us + 15000000), ESP_ERR_TIMEOUT);
    EXPECT_EQ(waited_ticks, 15000U);
    EXPECT_EQ(connects, 1);
    EXPECT_EQ(disconnects, 1);
    EXPECT_FALSE(wifi_manager_is_connected());
    got_ip();
    EXPECT_FALSE(wifi_manager_is_connected());
}
TEST_F(NetworkWake, FastIpEventIsNotLost)
{
    on_start = got_ip;
    EXPECT_EQ(wifi_manager_connect("test", "password", now_us + 1000000), ESP_OK);
    EXPECT_TRUE(wifi_manager_is_connected());
}
TEST_F(NetworkWake, WrongPasswordStopsImmediately)
{
    on_start = [] { disconnected(WIFI_REASON_AUTH_FAIL); };
    EXPECT_EQ(wifi_manager_connect("test", "wrong", now_us + 1000000), ESP_FAIL);
    EXPECT_EQ(connects, 1);
}
TEST_F(NetworkWake, MissingApRetriesAreFinite)
{
    on_wait = [] {
        for (int i = 0; i < 6; ++i)
            disconnected(WIFI_REASON_NO_AP_FOUND);
    };
    EXPECT_EQ(wifi_manager_connect("missing", "password", now_us + 1000000), ESP_FAIL);
    EXPECT_EQ(connects, 6);
}
TEST_F(NetworkWake, DisconnectAtDeadlineDoesNotRetry)
{
    on_wait = [] {
        now_us += 1000000;
        disconnected(WIFI_REASON_NO_AP_FOUND);
    };
    EXPECT_NE(wifi_manager_connect("test", "password", now_us + 1000000), ESP_OK);
    EXPECT_EQ(connects, 1);
}
TEST_F(NetworkWake, IntentionalDisconnectAndLaterExplicitConnect)
{
    on_start = got_ip;
    ASSERT_EQ(wifi_manager_connect("test", "password", now_us + 1000000), ESP_OK);
    ASSERT_EQ(wifi_manager_disconnect(), ESP_OK);
    EXPECT_EQ(connects, 1);
    EXPECT_FALSE(wifi_manager_is_connected());
    EXPECT_EQ(wifi_manager_connect("test", "password", now_us + 1000000), ESP_OK);
    EXPECT_EQ(connects, 2);
}
TEST_F(NetworkWake, ProvisioningKeepsApRunning)
{
    wifi_mode = WIFI_MODE_APSTA;
    on_wait = got_ip;
    EXPECT_EQ(wifi_manager_connect("test", "password", now_us + 15000000), ESP_OK);
    EXPECT_EQ(stops, 0);
    EXPECT_EQ(waited_ticks, 14900U);
}
TEST_F(NetworkWake, StartFailureReturnsWithoutWaiting)
{
    start_error = ESP_FAIL;
    EXPECT_EQ(wifi_manager_connect("test", "password", now_us + 1000000), ESP_FAIL);
    EXPECT_EQ(connects, 0);
}
TEST_F(NetworkWake, ExpiredDeadlineNeverStartsWifi)
{
    EXPECT_EQ(wifi_manager_connect("test", "password", now_us), ESP_ERR_TIMEOUT);
    EXPECT_EQ(connects, 0);
}
TEST_F(NetworkWake, OneAbsoluteDeadlineAndCleanupReserve)
{
    ASSERT_EQ(network_wake_begin(), ESP_OK);
    auto deadline = network_wake_deadline_us(1000000);
    now_us += 100000000;
    EXPECT_EQ(network_wake_deadline_us(1000000), deadline);
    EXPECT_EQ(network_wake_timeout_ms(1000000), 80000);
    now_us += 80000000;
    EXPECT_EQ(network_wake_timeout_ms(5000), 0);
}
TEST_F(NetworkWake, BackoffSurvivesSleepAndSuccessClearsIt)
{
    for (unsigned failures = 1; failures <= 20; ++failures) {
        ASSERT_EQ(network_wake_begin(), ESP_OK);
        EXPECT_LE(network_wake_backoff_seconds(), 21600U);
        reset_reason = ESP_RST_DEEPSLEEP;
        ASSERT_FALSE(network_wake_init());
    }
    EXPECT_EQ(network_wake_backoff_seconds(), 0U);  // Early re-sleep does not extend backoff.
    ASSERT_EQ(network_wake_begin(), ESP_OK);
    EXPECT_EQ(network_wake_backoff_seconds(), 21600U);
    network_wake_succeeded();
    EXPECT_EQ(network_wake_backoff_seconds(), 0U);
    network_wake_init();
    EXPECT_EQ(network_wake_backoff_seconds(), 0U);
}
TEST_F(NetworkWake, GuardRestartsOnceThenRequestsSleepRecovery)
{
    ASSERT_EQ(network_wake_begin(), ESP_OK);
    EXPECT_THROW(deadline_task(nullptr), Restart);
    EXPECT_GE(now_us, 301000000);
    reset_reason = ESP_RST_SW;
    EXPECT_TRUE(network_wake_init());
    EXPECT_FALSE(network_wake_active());
    EXPECT_EQ(network_wake_backoff_seconds(), 300U);
    reset_reason = ESP_RST_DEEPSLEEP;
    EXPECT_FALSE(network_wake_init());
}
TEST_F(NetworkWake, SupervisorAllocationFailureRequiresSleep)
{
    allocation_ok = false;
    EXPECT_EQ(network_wake_begin(), ESP_ERR_NO_MEM);
    EXPECT_EQ(network_wake_backoff_seconds(), 300U);
    EXPECT_EQ(connects, 0);
}
TEST_F(NetworkWake, UnrelatedResetDiscardsRetainedFailures)
{
    ASSERT_EQ(network_wake_begin(), ESP_OK);
    reset_reason = ESP_RST_PANIC;
    EXPECT_FALSE(network_wake_init());
    EXPECT_EQ(network_wake_backoff_seconds(), 0U);
}
TEST(NetworkPolicy, TerminalAndTransientHttpStatuses)
{
    for (int status : {200, 304, 301, 400, 401, 403, 404, 410, 422})
        EXPECT_FALSE(network_http_retryable(status)) << status;
    for (int status : {0, 408, 429, 500, 502, 503, 504})
        EXPECT_TRUE(network_http_retryable(status)) << status;
}
TEST(NetworkPolicy, BackoffSequenceAndDeadlineRounding)
{
    EXPECT_EQ(network_backoff_seconds(0), 0U);
    EXPECT_EQ(network_backoff_seconds(1), 300U);
    EXPECT_EQ(network_backoff_seconds(2), 600U);
    EXPECT_EQ(network_backoff_seconds(7), 19200U);
    EXPECT_EQ(network_backoff_seconds(255), 21600U);
    EXPECT_EQ(network_deadline_remaining_ms(999, 0, 5000), 0);
    EXPECT_EQ(network_deadline_remaining_ms(5000000, 0, 1000), 1000);
    EXPECT_EQ(network_deadline_remaining_ms(0, 1, 1000), 0);
}

TEST(NetworkPolicy, BackoffRespectsScheduleAndQuietHours)
{
    setenv("TZ", "UTC", 1);
    tzset();
    struct tm t = {};
    t.tm_year = 126;
    t.tm_mon = 8;
    t.tm_mday = 16;
    t.tm_hour = 19;
    t.tm_min = 55;
    time_t now = mktime(&t);
    cron_rule_t rule;
    ASSERT_TRUE(cron_parse("*/5 8-19 *", &rule));
    // A five-minute backoff must not wake at 20:00 outside the configured hours.
    EXPECT_EQ(network_next_wake_seconds(now, &rule, 1, 300), 12 * 3600 + 300);
    ASSERT_TRUE(cron_parse("*/5 * *", &rule));
    EXPECT_EQ(network_next_wake_seconds(now, &rule, 1, 300), 300);
    EXPECT_EQ(network_next_wake_seconds(now, &rule, 1, 301), 600);
    ASSERT_TRUE(cron_parse("0 8 *", &rule));
    EXPECT_EQ(network_next_wake_seconds(now, &rule, 1, 300), 12 * 3600 + 300);
    EXPECT_EQ(network_next_wake_seconds(now, nullptr, 0, 21600), 21600);
}
