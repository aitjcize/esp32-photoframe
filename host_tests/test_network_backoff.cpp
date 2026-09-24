// Backoff between failed unattended network wakes (#121), checked against the
// real cron engine: the wait grows and caps, only whole scheduled slots are
// skipped, and the schedule is never brought forward.

#include <gtest/gtest.h>

#include <cstdlib>
#include <ctime>
#include <vector>

extern "C" {
#include "cron.h"
#include "network_backoff.h"
}

namespace
{

constexpr int kFallback = 3600;

class NetworkBackoff : public ::testing::Test
{
   protected:
    void SetUp() override
    {
        setenv("TZ", "UTC0", 1);
        tzset();
    }

    static std::vector<cron_rule_t> compile(const std::vector<const char *> &exprs)
    {
        std::vector<cron_rule_t> rules;
        for (const char *e : exprs) {
            cron_rule_t r;
            EXPECT_TRUE(cron_parse(e, &r)) << e;
            rules.push_back(r);
        }
        return rules;
    }

    // 2026-09-24 hh:mm:ss UTC
    static time_t at(int hh, int mm, int ss)
    {
        struct tm t = {};
        t.tm_year = 2026 - 1900;
        t.tm_mon = 8;
        t.tm_mday = 24;
        t.tm_hour = hh;
        t.tm_min = mm;
        t.tm_sec = ss;
        t.tm_isdst = -1;
        return mktime(&t);
    }
};

TEST_F(NetworkBackoff, DelayDoublesFromFiveMinutesAndCaps)
{
    EXPECT_EQ(network_backoff_delay_sec(0), 0);
    EXPECT_EQ(network_backoff_delay_sec(1), 5 * 60);
    EXPECT_EQ(network_backoff_delay_sec(2), 10 * 60);
    EXPECT_EQ(network_backoff_delay_sec(3), 20 * 60);
    EXPECT_EQ(network_backoff_delay_sec(7), 320 * 60);
    EXPECT_EQ(network_backoff_delay_sec(8), NETWORK_BACKOFF_MAX_SEC);
    EXPECT_EQ(network_backoff_delay_sec(100), NETWORK_BACKOFF_MAX_SEC);
}

// An hourly frame that failed at 10:01 with a 5-minute backoff still waits
// for the 11:00 slot: backoff never wakes it earlier than the schedule.
TEST_F(NetworkBackoff, ShortDelayStillWaitsForNextSlot)
{
    auto rules = compile({"0 * *"});
    time_t now = at(10, 1, 30);
    time_t not_before = now + network_backoff_delay_sec(1);
    EXPECT_EQ(network_backoff_seconds_until_slot(now, not_before, rules.data(), 1, kFallback),
              at(11, 0, 0) - now);
}

// A backoff longer than the interval skips whole slots and lands on the
// first one at or after the hold.
TEST_F(NetworkBackoff, LongDelaySkipsSlots)
{
    auto rules = compile({"0 * *"});
    time_t now = at(10, 1, 30);
    time_t not_before = now + network_backoff_delay_sec(5);  // 80 min -> 11:21:30
    EXPECT_EQ(network_backoff_seconds_until_slot(now, not_before, rules.data(), 1, kFallback),
              at(12, 0, 0) - now);

    not_before = now + network_backoff_delay_sec(8);  // 6 h cap -> 16:01:30
    EXPECT_EQ(network_backoff_seconds_until_slot(now, not_before, rules.data(), 1, kFallback),
              at(17, 0, 0) - now);
}

// A hold that ends exactly on a slot uses that slot, not the one after.
TEST_F(NetworkBackoff, HoldEndingOnSlotUsesThatSlot)
{
    auto rules = compile({"0 * *"});
    time_t now = at(10, 30, 0);
    EXPECT_EQ(network_backoff_seconds_until_slot(now, at(11, 0, 0), rules.data(), 1, kFallback),
              at(11, 0, 0) - now);
}

// An expired hold (e.g. an early-wake re-sleep long after the failure) is
// just the ordinary next slot, so the re-sleep loop can't push the schedule
// out again and again.
TEST_F(NetworkBackoff, ExpiredHoldIsOrdinaryNextSlot)
{
    auto rules = compile({"0 * *"});
    time_t now = at(10, 59, 50);
    EXPECT_EQ(network_backoff_seconds_until_slot(now, at(10, 55, 0), rules.data(), 1, kFallback),
              10);
}

// The hold only ever makes the wait longer, on any schedule.
TEST_F(NetworkBackoff, NeverShorterThanSchedule)
{
    auto rules = compile({"0 */12 *"});
    time_t now = at(0, 0, 30);
    for (unsigned n = 0; n < 10; n++) {
        time_t not_before = now + network_backoff_delay_sec(n);
        EXPECT_GE(network_backoff_seconds_until_slot(now, not_before, rules.data(), 1, kFallback),
                  at(12, 0, 0) - now)
            << "failures=" << n;
    }
}

TEST_F(NetworkBackoff, NoRulesUsesDelayButNotBelowFallback)
{
    time_t now = at(10, 0, 0);
    EXPECT_EQ(network_backoff_seconds_until_slot(now, now + 300, nullptr, 0, kFallback), kFallback);
    EXPECT_EQ(network_backoff_seconds_until_slot(now, now + 7200, nullptr, 0, kFallback), 7200);
}

// The hour after a fall-back transition happens twice. A hold that ends in
// it must still land on a real slot at or after the hold, whichever
// occurrence it ends in (America/Los_Angeles, 2026-11-01 02:00 -> 01:00).
class NetworkBackoffDst : public NetworkBackoff
{
   protected:
    void SetUp() override
    {
        setenv("TZ", "America/Los_Angeles", 1);
        tzset();
    }

    // 2026-11-01 hh:mm UTC
    static time_t utc(int hh, int mm)
    {
        struct tm t = {};
        t.tm_year = 2026 - 1900;
        t.tm_mon = 10;
        t.tm_mday = 1;
        t.tm_hour = hh;
        t.tm_min = mm;
        return timegm(&t);
    }
};

TEST_F(NetworkBackoffDst, HoldEndingInRepeatedHourLandsOnRealSlot)
{
    auto rules = compile({"30 1 *"});
    time_t now = utc(7, 50);  // 00:50 PDT

    // Hold ends at 01:35 PDT, the first 01:35: next slot is 01:30 PST, the
    // second 01:30, at 09:30 UTC
    EXPECT_EQ(network_backoff_seconds_until_slot(now, utc(8, 35), rules.data(), 1, kFallback),
              utc(9, 30) - now);

    // Hold ends at 01:35 PST, the second 01:35: both 01:30s have passed, so
    // the next slot is the following day's, not a phantom 02:30 an hour on
    EXPECT_EQ(network_backoff_seconds_until_slot(now, utc(9, 35), rules.data(), 1, kFallback),
              utc(9, 30) + 24 * 3600 - now);
}

}  // namespace
