// The external RTC used to be written in local wall-clock time and is now
// written in UTC (#121). These tests pin down the one-off reinterpretation of
// an RTC that older firmware filled, under a DST-observing TZ rule.

#include <gtest/gtest.h>

#include <cstdlib>
#include <ctime>

extern "C" {
#include "rtc_clock.h"
}

namespace
{

// Central Europe: UTC+1, UTC+2 from the last Sunday of March to the last
// Sunday of October.
class RtcClockTest : public ::testing::Test
{
   protected:
    void SetUp() override
    {
        setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
        tzset();
    }
    void TearDown() override
    {
        unsetenv("TZ");
        tzset();
    }
};

// Calendar fields taken as UTC: what the driver returns for a given register
// content.
time_t fields_as_utc(int year, int mon, int mday, int hour, int min, int sec)
{
    struct tm t = {};
    t.tm_year = year - 1900;
    t.tm_mon = mon - 1;
    t.tm_mday = mday;
    t.tm_hour = hour;
    t.tm_min = min;
    t.tm_sec = sec;
    return timegm(&t);
}

}  // namespace

// Old firmware wrote 12:00 local in summer, i.e. 10:00 UTC.
TEST_F(RtcClockTest, SummerLocalFieldsAreTwoHoursAheadOfUtc)
{
    time_t stored = fields_as_utc(2026, 7, 1, 12, 0, 0);
    EXPECT_EQ(rtc_clock_from_legacy_local(stored), fields_as_utc(2026, 7, 1, 10, 0, 0));
}

// Old firmware wrote 12:00 local in winter, i.e. 11:00 UTC.
TEST_F(RtcClockTest, WinterLocalFieldsAreOneHourAheadOfUtc)
{
    time_t stored = fields_as_utc(2026, 1, 15, 12, 0, 0);
    EXPECT_EQ(rtc_clock_from_legacy_local(stored), fields_as_utc(2026, 1, 15, 11, 0, 0));
}

// The DST rule that applies is the one for the stored date, not for the
// date the frame last synced: a clock written on 2026-10-15 (summer) and read
// back on 2026-11-01 (winter) still denotes the instant it was written.
TEST_F(RtcClockTest, OffsetFollowsTheStoredDate)
{
    time_t written = fields_as_utc(2026, 10, 15, 6, 30, 0);  // the real instant
    struct tm local;
    localtime_r(&written, &local);
    EXPECT_EQ(local.tm_isdst, 1);
    EXPECT_EQ(local.tm_hour, 8);
    // What the old firmware put in the registers, read by the new driver.
    time_t stored = fields_as_utc(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                                  local.tm_hour, local.tm_min, local.tm_sec);
    EXPECT_EQ(rtc_clock_from_legacy_local(stored), written);
}

// Under a fixed-offset rule (no DST) the conversion is a plain shift.
TEST(RtcClockFixedOffsetTest, TaipeiIsEightHoursAhead)
{
    setenv("TZ", "CST-8", 1);
    tzset();
    time_t stored = fields_as_utc(2026, 11, 15, 20, 0, 0);
    EXPECT_EQ(rtc_clock_from_legacy_local(stored), fields_as_utc(2026, 11, 15, 12, 0, 0));
    unsetenv("TZ");
    tzset();
}

// UTC itself round-trips untouched: a frame on "UTC0" was never off.
TEST(RtcClockFixedOffsetTest, UtcIsUnchanged)
{
    setenv("TZ", "UTC0", 1);
    tzset();
    time_t stored = fields_as_utc(2026, 3, 29, 1, 30, 0);
    EXPECT_EQ(rtc_clock_from_legacy_local(stored), stored);
    unsetenv("TZ");
    tzset();
}
