#pragma once

#include <stdbool.h>
#include <stdint.h>

// Reconnect policy for the station link, driven from the WiFi event handler.
// Only an AP that keeps rejecting the credentials makes a connect fail -- the
// verdict an interactive boot answers by dropping the saved credentials. An AP
// that is absent, out of range or silent gets WIFI_MAX_RETRY reconnects in a
// bounded connect and unlimited ones in an unbounded (background) one, and
// running out of them says nothing about the credentials.
//
// Kept free of ESP-IDF headers (bar the reason codes) so it is host-tested.

// Reconnects after the first attempt before a bounded connect reports the AP
// unreachable, and consecutive rejections before the credentials are given up.
#define WIFI_MAX_RETRY 5

// A handshake that times out on a link weaker than this (dBm) is not held
// against the credentials: the EAPOL frames may simply have been lost.
#define WIFI_WEAK_LINK_RSSI (-80)

typedef enum {
    WIFI_RETRY_RECONNECT,    // try again
    WIFI_RETRY_UNREACHABLE,  // out of attempts without the AP accepting or rejecting us
    WIFI_RETRY_REJECTED,     // the AP refused the credentials WIFI_MAX_RETRY + 1 times in a row
    WIFI_RETRY_STOPPED,      // the caller stopped the attempt
} wifi_retry_verdict_t;

typedef struct {
    bool stopped;
    bool unbounded;
    int attempts;                  // reconnects since the last connect or association
    int auth_rejects;              // consecutive rejections; any other outcome resets it
    wifi_retry_verdict_t verdict;  // last decision
} wifi_retry_state_t;

// Fresh connect. Counters and a previous stop are cleared; the attempt limit
// (see wifi_retry_set_unbounded) is kept.
void wifi_retry_reset(wifi_retry_state_t *st);
// Disconnect reasons that mean the AP turned the credentials down, as opposed
// to it being absent or out of range. A wrong WPA2 passphrase surfaces as a
// 4-way handshake timeout, which a weak link can also produce; rssi is the
// link at disconnect (0 when unknown).
bool wifi_retry_is_auth_rejection(uint8_t reason, int8_t rssi);
// WIFI_EVENT_STA_DISCONNECTED: decide what to do next and remember it.
wifi_retry_verdict_t wifi_retry_on_disconnect(wifi_retry_state_t *st, uint8_t reason, int8_t rssi);
// WIFI_EVENT_STA_CONNECTED: the AP accepted the credentials.
void wifi_retry_on_connected(wifi_retry_state_t *st);
// Lift the attempt limit. Returns true when the last decision was
// WIFI_RETRY_UNREACHABLE, so nothing is in progress and the caller must start
// a new attempt itself. Not after wifi_retry_stop().
bool wifi_retry_set_unbounded(wifi_retry_state_t *st);
// Never reconnect again until the next wifi_retry_reset().
void wifi_retry_stop(wifi_retry_state_t *st);
