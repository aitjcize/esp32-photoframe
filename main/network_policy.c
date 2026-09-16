#include "network_policy.h"

int network_deadline_remaining_ms(int64_t deadline_us, int64_t now_us, int cap_ms)
{
    if (cap_ms <= 0 || deadline_us <= now_us) {
        return 0;
    }
    int64_t remaining = (deadline_us - now_us) / 1000;
    return remaining < cap_ms ? (int) remaining : cap_ms;
}

uint8_t network_failure_increment(uint8_t failures)
{
    return failures < NETWORK_FAILURE_LIMIT ? failures + 1 : NETWORK_FAILURE_LIMIT;
}

uint32_t network_backoff_seconds(uint8_t failures)
{
    if (failures == 0) {
        return 0;
    }
    if (failures > NETWORK_FAILURE_LIMIT) {
        failures = NETWORK_FAILURE_LIMIT;
    }
    uint32_t seconds = 300U << (failures - 1);
    return seconds < 21600U ? seconds : 21600U;
}

bool network_http_retryable(int status)
{
    return status == 0 || status == 408 || status == 429 || (status >= 500 && status <= 599);
}

int network_next_wake_seconds(time_t now, const cron_rule_t *rules, int count, uint32_t minimum_sec)
{
    if (count == 0) {
        return minimum_sec > CRON_FALLBACK_SEC ? (int) minimum_sec : CRON_FALLBACK_SEC;
    }
    // cron_seconds_until_next is strictly after its input. Include a boundary
    // exactly at the minimum while preserving configured quiet hours.
    uint32_t offset = minimum_sec > 0 ? minimum_sec - 1 : 0;
    now += offset;
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    return (int) offset + cron_seconds_until_next(&timeinfo, rules, count);
}
