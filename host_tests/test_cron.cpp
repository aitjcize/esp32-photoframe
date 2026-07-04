#include <gtest/gtest.h>

#include <cstdlib>
#include <ctime>
#include <vector>

extern "C" {
#include "cron.h"
}

namespace
{

// Build a struct tm for a local civil time. tm_isdst = -1 lets mktime resolve it.
struct tm make_tm(int year, int mon, int mday, int hour, int min, int sec)
{
    struct tm t = {};
    t.tm_year = year - 1900;
    t.tm_mon = mon - 1;
    t.tm_mday = mday;
    t.tm_hour = hour;
    t.tm_min = min;
    t.tm_sec = sec;
    t.tm_isdst = -1;
    return t;
}

std::vector<cron_rule_t> compile(const std::vector<const char *> &exprs)
{
    std::vector<cron_rule_t> rules;
    for (const char *e : exprs) {
        cron_rule_t r;
        EXPECT_TRUE(cron_parse(e, &r)) << "failed to parse: " << e;
        rules.push_back(r);
    }
    return rules;
}

int seconds_until(const struct tm &now, const std::vector<const char *> &exprs)
{
    auto rules = compile(exprs);
    return cron_seconds_until_next(&now, rules.data(), (int) rules.size());
}

class CronUtc : public ::testing::Test
{
   protected:
    void SetUp() override
    {
        setenv("TZ", "UTC0", 1);
        tzset();
    }
};

// 2026-06-27 is a Saturday. Format is 3-field: "minute hour day-of-week".
TEST_F(CronUtc, HourlyNextTopOfHour)
{
    EXPECT_EQ(seconds_until(make_tm(2026, 6, 27, 10, 0, 0), {"0 * *"}), 3600);
}

TEST_F(CronUtc, EveryTwelveHours)
{
    EXPECT_EQ(seconds_until(make_tm(2026, 6, 27, 10, 0, 0), {"0 */12 *"}), 7200);
}

TEST_F(CronUtc, EveryThirtyMinutes)
{
    EXPECT_EQ(seconds_until(make_tm(2026, 6, 27, 10, 0, 0), {"*/30 * *"}), 1800);
}

TEST_F(CronUtc, QuarterHourList)
{
    EXPECT_EQ(seconds_until(make_tm(2026, 6, 27, 10, 0, 0), {"0,15,30,45 * *"}), 900);
}

TEST_F(CronUtc, SpecificTimes)
{
    EXPECT_EQ(seconds_until(make_tm(2026, 6, 27, 10, 0, 0), {"0 9,12,18 *"}), 7200);
}

TEST_F(CronUtc, MultiRuleEarliestWins)
{
    EXPECT_EQ(seconds_until(make_tm(2026, 6, 27, 10, 0, 0), {"0 12 *", "30 10 *"}), 1800);
}

TEST_F(CronUtc, DayOfWeekMonday)
{
    // Sat 10:00 -> Mon 00:00 = 38h
    EXPECT_EQ(seconds_until(make_tm(2026, 6, 27, 10, 0, 0), {"0 0 1"}), 136800);
}

TEST_F(CronUtc, WeekdayRange)
{
    // Sat 10:00 -> Mon 09:00
    EXPECT_EQ(seconds_until(make_tm(2026, 6, 27, 10, 0, 0), {"0 9 1-5"}), 169200);
}

TEST_F(CronUtc, WeekendSundayAlias)
{
    // dow 0 = Sunday, 6 = Saturday; Sat 10:00 -> Sun 08:00
    EXPECT_EQ(seconds_until(make_tm(2026, 6, 27, 10, 0, 0), {"0 8 0,6"}), 79200);
}

TEST_F(CronUtc, SubMinuteRoundsUp)
{
    // now 10:00:30, next 11:00:00 -> 3570s
    EXPECT_EQ(seconds_until(make_tm(2026, 6, 27, 10, 0, 30), {"0 * *"}), 3570);
}

TEST_F(CronUtc, ImminentMatchHonored)
{
    // now 10:59:30: 11:00:00 is only 30s away and must still be scheduled.
    // Rotations never run ahead of their scheduled minute (early wakes
    // re-sleep the remainder), so an imminent match is a legitimate target.
    EXPECT_EQ(seconds_until(make_tm(2026, 6, 27, 10, 59, 30), {"0 * *"}), 30);
}

TEST_F(CronUtc, SpecificTimesDifferingMinutesSplitRules)
{
    // 9:15 (passed) + 12:45 -> next 12:45 = 2h45m = 9900s
    EXPECT_EQ(seconds_until(make_tm(2026, 6, 27, 10, 0, 0), {"15 9 *", "45 12 *"}), 9900);
}

TEST_F(CronUtc, EmptyRulesFallback)
{
    struct tm now = make_tm(2026, 6, 27, 10, 0, 0);
    EXPECT_EQ(cron_seconds_until_next(&now, nullptr, 0), CRON_FALLBACK_SEC);
}

// ---- Parser validation ----

TEST(CronParse, ValidExpressions)
{
    cron_rule_t r;
    EXPECT_TRUE(cron_parse("0 */12 *", &r));
    EXPECT_TRUE(cron_parse("*/30 8-22 1-5", &r));
    EXPECT_TRUE(cron_parse("0,15,30,45 * *", &r));
    EXPECT_TRUE(cron_parse("0 0 *", &r));
    EXPECT_TRUE(cron_parse("0 9,12,18 0,6", &r));
    EXPECT_TRUE(cron_parse("0 0 7", &r));         // 7 = Sunday alias
    EXPECT_TRUE(cron_parse("  0   *  *  ", &r));  // extra whitespace
    EXPECT_TRUE(cron_parse("5/10 * *", &r));      // step from bare value
    EXPECT_TRUE(cron_parse("0 0 5-7", &r));       // Vixie: Fri-Sun
    EXPECT_TRUE(cron_parse("0 0 0-7", &r));       // Vixie: every day
}

TEST(CronParse, SundayAliasInRanges)
{
    // Vixie semantics: 7 folds onto Sunday after the range expands.
    cron_rule_t r;
    ASSERT_TRUE(cron_parse("0 0 5-7", &r));
    EXPECT_EQ(r.dow, (uint8_t) 0b1100001);  // Sun, Fri, Sat
    ASSERT_TRUE(cron_parse("0 0 0-7", &r));
    EXPECT_EQ(r.dow, (uint8_t) 0b1111111);  // every day
    ASSERT_TRUE(cron_parse("0 0 7", &r));
    EXPECT_EQ(r.dow, (uint8_t) 0b0000001);  // Sunday only
}

TEST(CronParse, InvalidExpressions)
{
    cron_rule_t r;
    EXPECT_FALSE(cron_parse("", &r));
    EXPECT_FALSE(cron_parse("* *", &r));      // too few fields
    EXPECT_FALSE(cron_parse("* * * *", &r));  // too many fields
    EXPECT_FALSE(cron_parse("60 * *", &r));   // minute out of range
    EXPECT_FALSE(cron_parse("* 24 *", &r));   // hour out of range
    EXPECT_FALSE(cron_parse("* * 8", &r));    // dow out of range
    EXPECT_FALSE(cron_parse("5-2 * *", &r));  // inverted range
    EXPECT_FALSE(cron_parse("a * *", &r));    // non-numeric
    EXPECT_FALSE(cron_parse("*/0 * *", &r));  // zero step
}

TEST(CronParse, SundayAliasMatches)
{
    setenv("TZ", "UTC0", 1);
    tzset();
    cron_rule_t r;
    ASSERT_TRUE(cron_parse("0 0 7", &r));
    struct tm sunday = make_tm(2026, 6, 28, 0, 0, 0);  // 2026-06-28 is Sunday
    // normalize wday via mktime/localtime
    time_t tt = mktime(&sunday);
    struct tm local;
    localtime_r(&tt, &local);
    EXPECT_TRUE(cron_match(&r, &local));
}

// ---- DST (America/Los_Angeles) ----
// Verify we evaluate against local wall-clock and never fire inside a skipped hour.

struct InstantFields {
    int mon, mday, hour, min;
};

InstantFields next_instant(const struct tm &now, const std::vector<const char *> &exprs)
{
    auto rules = compile(exprs);
    struct tm base = now;
    base.tm_isdst = -1;
    time_t t0 = mktime(&base);
    int delta = cron_seconds_until_next(&now, rules.data(), (int) rules.size());
    time_t target = t0 + delta;
    struct tm lt;
    localtime_r(&target, &lt);
    return {lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min};
}

class CronDst : public ::testing::Test
{
   protected:
    void SetUp() override
    {
        setenv("TZ", "America/Los_Angeles", 1);
        tzset();
    }
};

TEST_F(CronDst, SpringForwardSkipsNonexistentTime)
{
    // 2026-03-08: clocks jump 02:00 -> 03:00, so 02:30 does not exist that day.
    // "30 2 *" must next fire on 2026-03-09 02:30, not during the gap.
    InstantFields got = next_instant(make_tm(2026, 3, 8, 0, 0, 0), {"30 2 *"});
    EXPECT_EQ(got.mon, 3);
    EXPECT_EQ(got.mday, 9);
    EXPECT_EQ(got.hour, 2);
    EXPECT_EQ(got.min, 30);
}

TEST_F(CronDst, FallBackFiresAtWallClock)
{
    // 2026-11-01: clocks fall 02:00 -> 01:00. "30 1 *" should fire at the
    // first 01:30 instant (the earliest match the scan encounters).
    InstantFields got = next_instant(make_tm(2026, 11, 1, 0, 0, 0), {"30 1 *"});
    EXPECT_EQ(got.mon, 11);
    EXPECT_EQ(got.mday, 1);
    EXPECT_EQ(got.hour, 1);
    EXPECT_EQ(got.min, 30);
}

}  // namespace
