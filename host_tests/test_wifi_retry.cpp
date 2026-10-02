// Reconnect policy for the station link: only an AP that keeps rejecting the
// credentials fails a connect. An absent one runs out of attempts in a
// bounded connect and is retried forever in an unbounded one, and never
// counts against the credentials.

#include <gtest/gtest.h>

extern "C" {
#include "esp_wifi_types.h"
#include "wifi_retry_policy.h"
}

namespace
{

constexpr int8_t kGoodRssi = -60;
constexpr int8_t kWeakRssi = -85;
constexpr int8_t kUnknownRssi = 0;

wifi_retry_state_t fresh(bool unbounded = false)
{
    wifi_retry_state_t st = {};
    wifi_retry_reset(&st);
    if (unbounded) {
        wifi_retry_set_unbounded(&st);
    }
    return st;
}

// Feed `n` identical disconnects and return the last verdict.
wifi_retry_verdict_t feed(wifi_retry_state_t *st, uint8_t reason, int8_t rssi, int n)
{
    wifi_retry_verdict_t v = WIFI_RETRY_RECONNECT;
    for (int i = 0; i < n; i++) {
        v = wifi_retry_on_disconnect(st, reason, rssi);
    }
    return v;
}

TEST(WifiRetry, ClassifiesOnlyCredentialRejections)
{
    EXPECT_TRUE(wifi_retry_is_auth_rejection(WIFI_REASON_AUTH_FAIL, kGoodRssi));
    EXPECT_TRUE(wifi_retry_is_auth_rejection(WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT, kGoodRssi));
    EXPECT_TRUE(wifi_retry_is_auth_rejection(WIFI_REASON_HANDSHAKE_TIMEOUT, kGoodRssi));

    EXPECT_FALSE(wifi_retry_is_auth_rejection(WIFI_REASON_NO_AP_FOUND, kGoodRssi));
    EXPECT_FALSE(wifi_retry_is_auth_rejection(WIFI_REASON_BEACON_TIMEOUT, kGoodRssi));
    EXPECT_FALSE(wifi_retry_is_auth_rejection(WIFI_REASON_ASSOC_LEAVE, kGoodRssi));
    EXPECT_FALSE(wifi_retry_is_auth_rejection(WIFI_REASON_AUTH_EXPIRE, kGoodRssi));
    EXPECT_FALSE(wifi_retry_is_auth_rejection(WIFI_REASON_ASSOC_FAIL, kGoodRssi));
    EXPECT_FALSE(wifi_retry_is_auth_rejection(WIFI_REASON_CONNECTION_FAIL, kGoodRssi));
}

// A handshake can time out because the EAPOL frames were lost, not because
// the password was wrong; on a weak link it is not held against the
// credentials. AUTH_FAIL is a verdict from the AP and always counts.
TEST(WifiRetry, HandshakeTimeoutOnWeakLinkIsNotARejection)
{
    EXPECT_FALSE(wifi_retry_is_auth_rejection(WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT, kWeakRssi));
    EXPECT_FALSE(wifi_retry_is_auth_rejection(WIFI_REASON_HANDSHAKE_TIMEOUT, kWeakRssi));
    EXPECT_TRUE(
        wifi_retry_is_auth_rejection(WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT, WIFI_WEAK_LINK_RSSI));
    EXPECT_TRUE(wifi_retry_is_auth_rejection(WIFI_REASON_AUTH_FAIL, kWeakRssi));
    // No RSSI reported: no reason to doubt the rejection.
    EXPECT_TRUE(wifi_retry_is_auth_rejection(WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT, kUnknownRssi));
}

// The bug: an AP that is absent (router still booting after the power cut
// that rebooted the frame) exhausts the attempts in seconds. That must never
// read as a credential failure.
TEST(WifiRetry, AbsentApRunsOutOfAttemptsWithoutRejecting)
{
    wifi_retry_state_t st = fresh();
    for (int i = 0; i < WIFI_MAX_RETRY; i++) {
        EXPECT_EQ(wifi_retry_on_disconnect(&st, WIFI_REASON_NO_AP_FOUND, kUnknownRssi),
                  WIFI_RETRY_RECONNECT)
            << "attempt " << i;
    }
    EXPECT_EQ(wifi_retry_on_disconnect(&st, WIFI_REASON_NO_AP_FOUND, kUnknownRssi),
              WIFI_RETRY_UNREACHABLE);
    EXPECT_EQ(st.verdict, WIFI_RETRY_UNREACHABLE);
    EXPECT_EQ(st.auth_rejects, 0);
}

TEST(WifiRetry, UnboundedRetriesAnAbsentApForever)
{
    wifi_retry_state_t st = fresh(true);
    EXPECT_EQ(feed(&st, WIFI_REASON_NO_AP_FOUND, kUnknownRssi, 1000), WIFI_RETRY_RECONNECT);
    EXPECT_EQ(feed(&st, WIFI_REASON_BEACON_TIMEOUT, kGoodRssi, 1000), WIFI_RETRY_RECONNECT);
}

TEST(WifiRetry, WrongPasswordIsRejectedAfterMaxRetryInBothModes)
{
    for (bool unbounded : {false, true}) {
        wifi_retry_state_t st = fresh(unbounded);
        EXPECT_EQ(feed(&st, WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT, kGoodRssi, WIFI_MAX_RETRY),
                  WIFI_RETRY_RECONNECT)
            << "unbounded " << unbounded;
        EXPECT_EQ(wifi_retry_on_disconnect(&st, WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT, kGoodRssi),
                  WIFI_RETRY_REJECTED)
            << "unbounded " << unbounded;
    }
}

// Only an unbroken run of rejections counts: any other failure in between
// starts the count over.
TEST(WifiRetry, RejectionsMustBeConsecutive)
{
    wifi_retry_state_t st = fresh(true);
    for (int round = 0; round < 10; round++) {
        EXPECT_EQ(feed(&st, WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT, kGoodRssi, WIFI_MAX_RETRY),
                  WIFI_RETRY_RECONNECT);
        EXPECT_EQ(wifi_retry_on_disconnect(&st, WIFI_REASON_NO_AP_FOUND, kUnknownRssi),
                  WIFI_RETRY_RECONNECT);
    }
    EXPECT_EQ(st.auth_rejects, 0);
}

// A bounded connect on a weak link: handshake timeouts count as attempts,
// not as rejections, so it ends as unreachable (ESP_ERR_TIMEOUT: credentials
// kept) rather than rejected.
TEST(WifiRetry, WeakLinkHandshakeTimeoutsEndUnreachableNotRejected)
{
    wifi_retry_state_t st = fresh();
    EXPECT_EQ(feed(&st, WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT, kWeakRssi, WIFI_MAX_RETRY + 1),
              WIFI_RETRY_UNREACHABLE);

    wifi_retry_state_t bg = fresh(true);
    EXPECT_EQ(feed(&bg, WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT, kWeakRssi, 100), WIFI_RETRY_RECONNECT);
}

TEST(WifiRetry, AssociationResetsBothCounts)
{
    wifi_retry_state_t st = fresh();
    feed(&st, WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT, kGoodRssi, WIFI_MAX_RETRY);
    EXPECT_EQ(st.attempts, WIFI_MAX_RETRY);
    EXPECT_EQ(st.auth_rejects, WIFI_MAX_RETRY);

    wifi_retry_on_connected(&st);
    EXPECT_EQ(st.attempts, 0);
    EXPECT_EQ(st.auth_rejects, 0);
    EXPECT_EQ(feed(&st, WIFI_REASON_BEACON_TIMEOUT, kGoodRssi, WIFI_MAX_RETRY),
              WIFI_RETRY_RECONNECT);
}

TEST(WifiRetry, StopEndsReconnects)
{
    wifi_retry_state_t st = fresh(true);
    wifi_retry_stop(&st);
    EXPECT_EQ(st.verdict, WIFI_RETRY_STOPPED);
    EXPECT_EQ(wifi_retry_on_disconnect(&st, WIFI_REASON_ASSOC_LEAVE, kGoodRssi),
              WIFI_RETRY_STOPPED);
    EXPECT_EQ(wifi_retry_on_disconnect(&st, WIFI_REASON_NO_AP_FOUND, kUnknownRssi),
              WIFI_RETRY_STOPPED);

    // The next connect starts over.
    wifi_retry_reset(&st);
    EXPECT_EQ(wifi_retry_on_disconnect(&st, WIFI_REASON_NO_AP_FOUND, kUnknownRssi),
              WIFI_RETRY_RECONNECT);
}

// Lifting the limit while the handler is still retrying changes nothing;
// after it stopped as unreachable the caller has to start a new attempt.
TEST(WifiRetry, SetUnboundedRestartsOnlyAfterUnreachable)
{
    wifi_retry_state_t running = fresh();
    feed(&running, WIFI_REASON_NO_AP_FOUND, kUnknownRssi, 2);
    EXPECT_FALSE(wifi_retry_set_unbounded(&running));
    EXPECT_EQ(feed(&running, WIFI_REASON_NO_AP_FOUND, kUnknownRssi, 100), WIFI_RETRY_RECONNECT);

    wifi_retry_state_t exhausted = fresh();
    feed(&exhausted, WIFI_REASON_NO_AP_FOUND, kUnknownRssi, WIFI_MAX_RETRY + 1);
    EXPECT_EQ(exhausted.verdict, WIFI_RETRY_UNREACHABLE);
    EXPECT_TRUE(wifi_retry_set_unbounded(&exhausted));
    EXPECT_EQ(exhausted.verdict, WIFI_RETRY_RECONNECT);
    EXPECT_FALSE(wifi_retry_set_unbounded(&exhausted));
    EXPECT_EQ(feed(&exhausted, WIFI_REASON_NO_AP_FOUND, kUnknownRssi, 100), WIFI_RETRY_RECONNECT);

    // A rejection verdict is final for this attempt: nothing to restart.
    wifi_retry_state_t rejected = fresh();
    feed(&rejected, WIFI_REASON_AUTH_FAIL, kGoodRssi, WIFI_MAX_RETRY + 1);
    EXPECT_EQ(rejected.verdict, WIFI_RETRY_REJECTED);
    EXPECT_FALSE(wifi_retry_set_unbounded(&rejected));
}

// A connect started later in the same session (credentials changed from the
// web UI) keeps the unbounded mode an interactive boot chose.
TEST(WifiRetry, ResetKeepsUnboundedMode)
{
    wifi_retry_state_t st = fresh(true);
    feed(&st, WIFI_REASON_NO_AP_FOUND, kUnknownRssi, 50);
    wifi_retry_reset(&st);
    EXPECT_EQ(st.attempts, 0);
    EXPECT_EQ(feed(&st, WIFI_REASON_NO_AP_FOUND, kUnknownRssi, 50), WIFI_RETRY_RECONNECT);
}

}  // namespace
