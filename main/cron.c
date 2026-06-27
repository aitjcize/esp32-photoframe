#include "cron.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

// Parse a non-negative integer from *pp, advancing the pointer. Returns false
// if no digits are present.
static bool parse_uint(const char **pp, int *out)
{
    const char *p = *pp;
    if (!isdigit((unsigned char) *p)) {
        return false;
    }
    int v = 0;
    while (isdigit((unsigned char) *p)) {
        v = v * 10 + (*p - '0');
        if (v > 100000) {  // guard against overflow on absurd input
            return false;
        }
        p++;
    }
    *out = v;
    *pp = p;
    return true;
}

// Parse one cron field (which may contain comma-separated terms) into a bitmask
// covering [lo, hi]. When `is_dow` is true, the value 7 is folded onto 0 (Sunday).
// Returns false on any malformed term or out-of-range value.
static bool parse_field(const char *field, int lo, int hi, bool is_dow, uint64_t *mask_out)
{
    uint64_t mask = 0;
    const char *p = field;

    if (*p == '\0') {
        return false;
    }

    while (1) {
        int start = lo;
        int end = hi;
        int step = 1;
        bool is_star = false;
        bool is_range = false;

        if (*p == '*') {
            p++;
            is_star = true;  // start/end already span the full range
        } else {
            if (!parse_uint(&p, &start)) {
                return false;
            }
            if (*p == '-') {
                p++;
                is_range = true;
                if (!parse_uint(&p, &end)) {
                    return false;
                }
            } else {
                end = start;  // single value (unless a step follows, handled below)
            }
        }

        if (*p == '/') {
            p++;
            if (!parse_uint(&p, &step) || step <= 0) {
                return false;
            }
            // A bare number followed by a step ("5/10") means "from 5 through hi".
            if (!is_star && !is_range) {
                end = hi;
            }
        }

        if (is_dow && start == 7) {
            start = 0;
        }
        if (is_dow && end == 7) {
            end = 0;
        }

        if (start < lo || end > hi || start > end) {
            return false;
        }

        for (int v = start; v <= end; v += step) {
            mask |= (1ULL << v);
        }

        if (*p == ',') {
            p++;
            continue;
        }
        break;
    }

    if (*p != '\0') {
        return false;  // trailing garbage
    }

    *mask_out = mask;
    return true;
}

bool cron_parse(const char *expr, cron_rule_t *out)
{
    if (!expr || !out) {
        return false;
    }

    // Split into up to 3 fields on runs of whitespace.
    char fields[3][32];
    int n = 0;
    const char *p = expr;

    while (*p && n < 3) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        int len = 0;
        while (*p && *p != ' ' && *p != '\t') {
            if (len >= (int) sizeof(fields[0]) - 1) {
                return false;  // field too long
            }
            fields[n][len++] = *p++;
        }
        fields[n][len] = '\0';
        n++;
    }
    // Skip trailing whitespace; reject if there is a 4th field.
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (n != 3 || *p != '\0') {
        return false;
    }

    uint64_t minute_mask, hour_mask, dow_mask;
    if (!parse_field(fields[0], 0, 59, false, &minute_mask)) {
        return false;
    }
    if (!parse_field(fields[1], 0, 23, false, &hour_mask)) {
        return false;
    }
    if (!parse_field(fields[2], 0, 6, true, &dow_mask)) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->minute = minute_mask;
    out->hour = (uint32_t) hour_mask;
    out->dow = (uint8_t) dow_mask;
    return true;
}

bool cron_match(const cron_rule_t *r, const struct tm *t)
{
    if (!(r->minute & (1ULL << t->tm_min))) {
        return false;
    }
    if (!(r->hour & (1UL << t->tm_hour))) {
        return false;
    }
    return (r->dow & (1UL << t->tm_wday)) != 0;
}

// True if the local time `t` falls inside the quiet-hours window.
static bool in_quiet_window(const struct tm *t, const sleep_schedule_config_t *sleep)
{
    if (!sleep || !sleep->enabled) {
        return false;
    }
    int cur = t->tm_hour * 60 + t->tm_min;
    if (sleep->start_minutes > sleep->end_minutes) {
        // Crosses midnight (e.g. 23:00 - 07:00).
        return cur >= sleep->start_minutes || cur < sleep->end_minutes;
    }
    // Same-day window (empty when start == end).
    return cur >= sleep->start_minutes && cur < sleep->end_minutes;
}

int cron_seconds_until_next(const struct tm *now, const cron_rule_t *rules, int n_rules,
                            const sleep_schedule_config_t *sleep)
{
    if (!now || !rules || n_rules <= 0) {
        return CRON_FALLBACK_SEC;
    }

    struct tm base = *now;
    base.tm_isdst = -1;  // let mktime resolve DST for the starting instant
    time_t t0 = mktime(&base);
    if (t0 == (time_t) -1) {
        return CRON_FALLBACK_SEC;
    }

    // Start scanning at the next whole minute boundary.
    time_t start = t0 - (t0 % 60) + 60;

    for (time_t t = start; t <= t0 + CRON_MAX_HORIZON_SEC; t += 60) {
        struct tm cand;
        localtime_r(&t, &cand);

        bool matched = false;
        for (int i = 0; i < n_rules; i++) {
            if (cron_match(&rules[i], &cand)) {
                matched = true;
                break;
            }
        }
        if (!matched) {
            continue;
        }
        if (in_quiet_window(&cand, sleep)) {
            continue;
        }

        int delta = (int) (t - t0);
        if (delta < CRON_MIN_DELAY_SEC) {
            continue;  // too soon (clock drift guard) — wait for the following match
        }
        return delta;
    }

    return CRON_FALLBACK_SEC;
}
