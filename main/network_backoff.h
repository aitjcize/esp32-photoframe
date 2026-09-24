#pragma once

#include <time.h>

#include "cron.h"

// Backoff between unattended wakes whose network work failed (#121). A frame
// that can't reach its server -- WiFi out of range, server down, name not
// resolving -- otherwise pays for a full connect-and-fetch attempt on every
// scheduled slot, which is what turns months of battery into weeks. After n
// consecutive failures the next attempt waits at least
// NETWORK_BACKOFF_BASE_SEC * 2^(n-1), capped at NETWORK_BACKOFF_MAX_SEC, and
// then lands on the first scheduled slot at or after that instant: the
// schedule is never brought forward, only slots are skipped. One success
// (a displayed image, a 304, or a valid Home Assistant answer) clears it, and
// so does any interactive wake: the owner is there and may have fixed things.
//
// Kept free of ESP-IDF headers so the policy is host-tested against the real
// cron engine. The consecutive-failure count itself lives with the caller.

#define NETWORK_BACKOFF_BASE_SEC (5 * 60)
#define NETWORK_BACKOFF_MAX_SEC (6 * 60 * 60)

// Minimum wait after `failures` consecutive failed wakes; 0 for none.
int network_backoff_delay_sec(unsigned failures);

// Seconds from `now` until the first scheduled slot at or after `not_before`.
// With `not_before` in the past this is the ordinary next slot. Without rules
// the wait is the plain delay, but never less than `fallback_sec` (the
// no-schedule interval the caller would have used anyway).
int network_backoff_seconds_until_slot(time_t now, time_t not_before, const cron_rule_t *rules,
                                       int n_rules, int fallback_sec);
