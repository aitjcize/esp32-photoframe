#include "wifi_retry_policy.h"

#include "esp_wifi_types.h"

void wifi_retry_reset(wifi_retry_state_t *st)
{
    st->stopped = false;
    st->attempts = 0;
    st->auth_rejects = 0;
    st->verdict = WIFI_RETRY_RECONNECT;
}

bool wifi_retry_is_auth_rejection(uint8_t reason, int8_t rssi)
{
    switch (reason) {
    case WIFI_REASON_AUTH_FAIL:
        return true;
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return rssi == 0 || rssi >= WIFI_WEAK_LINK_RSSI;
    default:
        return false;
    }
}

wifi_retry_verdict_t wifi_retry_on_disconnect(wifi_retry_state_t *st, uint8_t reason, int8_t rssi)
{
    if (wifi_retry_is_auth_rejection(reason, rssi)) {
        st->auth_rejects++;
    } else {
        st->auth_rejects = 0;
    }

    wifi_retry_verdict_t verdict;
    if (st->stopped) {
        verdict = WIFI_RETRY_STOPPED;
    } else if (st->auth_rejects > WIFI_MAX_RETRY) {
        verdict = WIFI_RETRY_REJECTED;
    } else if (!st->unbounded && st->attempts >= WIFI_MAX_RETRY) {
        verdict = WIFI_RETRY_UNREACHABLE;
    } else {
        st->attempts++;
        verdict = WIFI_RETRY_RECONNECT;
    }
    st->verdict = verdict;
    return verdict;
}

void wifi_retry_on_connected(wifi_retry_state_t *st)
{
    st->attempts = 0;
    st->auth_rejects = 0;
}

bool wifi_retry_set_unbounded(wifi_retry_state_t *st)
{
    st->unbounded = true;
    if (st->verdict != WIFI_RETRY_UNREACHABLE) {
        return false;
    }
    st->verdict = WIFI_RETRY_RECONNECT;
    st->attempts = 0;
    return true;
}

void wifi_retry_stop(wifi_retry_state_t *st)
{
    st->stopped = true;
    st->verdict = WIFI_RETRY_STOPPED;
}
