#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "cron.h"

// One scheduled pipeline, including display and teardown. Network work gets
// 3 minutes; reserve 2 minutes for decode, panel refresh and sleep teardown.
// The supervisor recovers a stuck pipeline at 5 minutes.
#define NETWORK_WAKE_LIMIT_MS 300000
#define NETWORK_WAKE_FINISH_MS 120000
#define NETWORK_FAILURE_LIMIT 8

int network_deadline_remaining_ms(int64_t deadline_us, int64_t now_us, int cap_ms);
uint8_t network_failure_increment(uint8_t failures);
uint32_t network_backoff_seconds(uint8_t failures);
bool network_http_retryable(int status);
int network_next_wake_seconds(time_t now, const cron_rule_t *rules, int count,
                              uint32_t minimum_sec);
