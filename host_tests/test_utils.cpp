/**
 * Google Test-based unit tests for calculate_next_wakeup_interval
 */

#include <gtest/gtest.h>
#include <time.h>

#include <cstring>

#include "../main/testable_utils.h"

// Test fixture
class CalculateNextWakeupIntervalTest : public ::testing::Test
{
   protected:
    void SetUp() override
    {
        // Reset config before each test
        config.enabled = false;
        config.start_minutes = 1380;  // 23:00
        config.end_minutes = 420;     // 07:00
        SetMockTime(0, 0, 0);
    }

    void SetMockTime(int hour, int minute, int second)
    {
        memset(&timeinfo, 0, sizeof(timeinfo));
        timeinfo.tm_hour = hour;
        timeinfo.tm_min = minute;
        timeinfo.tm_sec = second;
        timeinfo.tm_year = 126;  // 2026
        timeinfo.tm_mon = 0;     // January
        timeinfo.tm_mday = 20;
    }

    struct tm timeinfo;
    sleep_schedule_config_t config;
};

// Test Case 1: No sleep schedule - simple clock alignment
TEST_F(CalculateNextWakeupIntervalTest, NoSleepSchedule1HourInterval)
{
    config.enabled = false;
    SetMockTime(10, 30, 0);

    int result = calculate_next_wakeup_interval(&timeinfo, 3600, true, &config);

    EXPECT_EQ(1800, result) << "Should wake in 30 minutes (at 11:00)";
}

// Test Case 2: No sleep schedule - 30 minute interval
TEST_F(CalculateNextWakeupIntervalTest, NoSleepSchedule30MinInterval)
{
    config.enabled = false;
    SetMockTime(10, 15, 0);

    int result = calculate_next_wakeup_interval(&timeinfo, 1800, true, &config);

    EXPECT_EQ(900, result) << "Should wake in 15 minutes (at 10:30)";
}

// Test Case 3: Sleep schedule enabled, next wake-up is outside schedule
TEST_F(CalculateNextWakeupIntervalTest, SleepScheduleWakeOutside)
{
    config.enabled = true;
    config.start_minutes = 1380;  // 23:00
    config.end_minutes = 420;     // 07:00
    SetMockTime(18, 0, 0);

    int result = calculate_next_wakeup_interval(&timeinfo, 3600, true, &config);

    EXPECT_EQ(3600, result) << "Should wake in 1 hour (at 19:00)";
}

// Test Case 4: Sleep schedule enabled, next wake-up would be inside schedule
TEST_F(CalculateNextWakeupIntervalTest, SleepScheduleWakeInside)
{
    config.enabled = true;
    config.start_minutes = 1380;  // 23:00
    config.end_minutes = 420;     // 07:00
    SetMockTime(22, 30, 0);

    int result = calculate_next_wakeup_interval(&timeinfo, 3600, true, &config);

    EXPECT_EQ(30600, result)
        << "Should skip to 07:00 next day (8.5 hours) - sleep_end is exclusive";
}

// Test Case 5: Currently in sleep schedule
TEST_F(CalculateNextWakeupIntervalTest, CurrentlyInSleepSchedule)
{
    config.enabled = true;
    config.start_minutes = 1380;  // 23:00
    config.end_minutes = 420;     // 07:00
    SetMockTime(2, 0, 0);

    int result = calculate_next_wakeup_interval(&timeinfo, 3600, true, &config);

    EXPECT_EQ(18000, result) << "Should wake at 07:00 (5 hours) - sleep_end is exclusive";
}

// Test Case 6: Sleep schedule ends at aligned time
TEST_F(CalculateNextWakeupIntervalTest, SleepScheduleEndsAtAlignedTime)
{
    config.enabled = true;
    config.start_minutes = 1380;  // 23:00
    config.end_minutes = 420;     // 07:00
    SetMockTime(6, 0, 0);

    int result = calculate_next_wakeup_interval(&timeinfo, 3600, true, &config);

    EXPECT_EQ(3600, result) << "Should wake at 07:00 (1 hour)";
}

// Test Case 7: Sleep schedule with 2-hour interval
TEST_F(CalculateNextWakeupIntervalTest, SleepSchedule2HourInterval)
{
    config.enabled = true;
    config.start_minutes = 1380;  // 23:00
    config.end_minutes = 435;     // 07:15
    SetMockTime(22, 0, 0);

    int result = calculate_next_wakeup_interval(&timeinfo, 7200, true, &config);

    EXPECT_EQ(36000, result)
        << "Should skip to 08:00 next day (10 hours) - first aligned time >= sleep_end";
}

// Test Case 8: Same-day schedule (not overnight)
TEST_F(CalculateNextWakeupIntervalTest, SameDaySchedule)
{
    config.enabled = true;
    config.start_minutes = 720;  // 12:00
    config.end_minutes = 840;    // 14:00
    SetMockTime(11, 30, 0);

    int result = calculate_next_wakeup_interval(&timeinfo, 3600, true, &config);

    EXPECT_EQ(9000, result) << "Should skip to 14:00 (2.5 hours) - sleep_end is exclusive";
}

// Test Case 9: Edge case - exactly at midnight
TEST_F(CalculateNextWakeupIntervalTest, ExactlyAtMidnight)
{
    config.enabled = true;
    config.start_minutes = 1380;  // 23:00
    config.end_minutes = 420;     // 07:00
    SetMockTime(0, 0, 0);

    int result = calculate_next_wakeup_interval(&timeinfo, 3600, true, &config);

    EXPECT_EQ(25200, result) << "Should wake at 07:00 (7 hours) - sleep_end is exclusive";
}

// Test Case 10: 15-minute interval
TEST_F(CalculateNextWakeupIntervalTest, FifteenMinuteInterval)
{
    config.enabled = false;
    SetMockTime(10, 7, 0);

    int result = calculate_next_wakeup_interval(&timeinfo, 900, true, &config);

    EXPECT_EQ(480, result) << "Should wake at 10:15 (8 minutes)";
}

// Test Case 11: Time drift - woke up 40 seconds early, should skip to next interval
TEST_F(CalculateNextWakeupIntervalTest, TimeDriftWokeUpEarly)
{
    config.enabled = false;
    SetMockTime(16, 59, 20);

    int result = calculate_next_wakeup_interval(&timeinfo, 3600, true, &config);

    EXPECT_EQ(3640, result) << "Should skip to 18:00 since 40s < 60s threshold";
}

// New tests for Non-Aligned mode
TEST_F(CalculateNextWakeupIntervalTest, NonAlignedWakeOutsideSchedule)
{
    config.enabled = true;
    config.start_minutes = 1380;  // 23:00
    config.end_minutes = 420;     // 07:00
    SetMockTime(18, 5, 0);

    int result = calculate_next_wakeup_interval(&timeinfo, 3600, false, &config);

    EXPECT_EQ(3600, result) << "Should wake exactly in 1 hour (at 19:05)";
}

TEST_F(CalculateNextWakeupIntervalTest, NonAlignedWakeInsideSchedule)
{
    config.enabled = true;
    config.start_minutes = 1380;  // 23:00
    config.end_minutes = 420;     // 07:00
    SetMockTime(22, 30, 0);

    int result = calculate_next_wakeup_interval(&timeinfo, 3600, false, &config);

    // 22:30 + 1 hour = 23:30 (inside schedule)
    // Should wake at 07:00 next day (8.5 hours = 30600 seconds)
    EXPECT_EQ(30600, result) << "Should skip to 07:00 next day";
}

TEST_F(CalculateNextWakeupIntervalTest, NonAlignedCurrentlyInSchedule)
{
    config.enabled = true;
    config.start_minutes = 1380;  // 23:00
    config.end_minutes = 420;     // 07:00
    SetMockTime(2, 0, 0);         // Currently 02:00 (inside schedule)

    int result = calculate_next_wakeup_interval(&timeinfo, 3600, false, &config);

    EXPECT_EQ(18000, result) << "Should wake at 07:00 (5 hours)";
}

TEST_F(CalculateNextWakeupIntervalTest, NonAlignedOvernightCrossMidnight)
{
    config.enabled = true;
    config.start_minutes = 1380;  // 23:00
    config.end_minutes = 420;     // 07:00
    SetMockTime(22, 30, 0);       // 22:30, 30 mins before sleep schedule

    // Interval is 1 hour, so next wake at 23:30 (inside schedule)
    int result = calculate_next_wakeup_interval(&timeinfo, 3600, false, &config);

    EXPECT_EQ(30600, result) << "Should wake at 07:00 next day (8.5 hours)";
}

TEST_F(CalculateNextWakeupIntervalTest, NonAlignedSameDaySchedule)
{
    config.enabled = true;
    config.start_minutes = 720;  // 12:00
    config.end_minutes = 840;    // 14:00
    SetMockTime(11, 30, 0);      // 11:30, 30 mins before sleep schedule

    // Interval is 1 hour, so next wake at 12:30 (inside schedule)
    int result = calculate_next_wakeup_interval(&timeinfo, 3600, false, &config);

    EXPECT_EQ(9000, result) << "Should wake at 14:00 (2.5 hours)";
}

TEST_F(CalculateNextWakeupIntervalTest, SameDayScheduleWraparound)
{
    config.enabled = true;
    config.start_minutes = 0;  // 00:00
    config.end_minutes = 480;  // 08:00
    SetMockTime(23, 40, 51);   // Current time 23:40:51

    // Rotation interval 1 hour aligned
    // Next aligned time would be 00:00 (tomorrow)
    // 00:00 falls in schedule [00:00, 08:00)
    // So should skip to 08:00 tomorrow.

    int result = calculate_next_wakeup_interval(&timeinfo, 3600, true, &config);

    // 23:40:51 to 08:00:00 next day
    // 23:40:51 -> 24:00:00 = 19m 9s = 1149s
    // 00:00:00 -> 08:00:00 = 8h = 28800s
    // Total = 29949s
    EXPECT_EQ(29949, result) << "Should wake at 08:00 tomorrow (wrapper around)";
}

int main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
