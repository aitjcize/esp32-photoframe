#include "network_backoff.h"

int network_backoff_delay_sec(unsigned failures)
{
    if (failures == 0) {
        return 0;
    }
    int delay = NETWORK_BACKOFF_BASE_SEC;
    for (unsigned i = 1; i < failures && delay < NETWORK_BACKOFF_MAX_SEC; i++) {
        delay *= 2;
    }
    return delay > NETWORK_BACKOFF_MAX_SEC ? NETWORK_BACKOFF_MAX_SEC : delay;
}

int network_backoff_seconds_until_slot(time_t now, time_t not_before, const cron_rule_t *rules,
                                       int n_rules, int fallback_sec)
{
    if (not_before < now) {
        not_before = now;
    }
    int held = (int) (not_before - now);

    if (!rules || n_rules <= 0) {
        return held > fallback_sec ? held : fallback_sec;
    }

    // cron_seconds_until_next() scans from the whole minute after its base,
    // so base one second early to keep a slot that falls exactly on
    // not_before.
    time_t base = not_before - 1;
    struct tm at;
    localtime_r(&base, &at);
    return held - 1 + cron_seconds_until_next(&at, rules, n_rules);
}
