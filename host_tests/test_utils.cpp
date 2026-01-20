/**
 * Google Test-based unit tests for calculate_next_aligned_wakeup
 */

#include <gtest/gtest.h>
#include <time.h>
#include <cstring>

// Forward declaration
extern "C" int calculate_next_aligned_wakeup(int rotate_interval);

// Mock data
static bool mock_sleep_schedule_enabled = false;
static int mock_sleep_start = 1380;  // 23:00
static int mock_sleep_end = 420;     // 07:00
static struct tm mock_timeinfo;

// Mock implementations
extern "C" {
    bool config_manager_get_sleep_schedule_enabled(void) {
        return mock_sleep_schedule_enabled;
    }

    int config_manager_get_sleep_schedule_start(void) {
        return mock_sleep_start;
    }

    int config_manager_get_sleep_schedule_end(void) {
        return mock_sleep_end;
    }

    // Override time functions
    time_t time(time_t *t) {
        time_t mock_time = mktime(&mock_timeinfo);
        if (t != NULL) {
            *t = mock_time;
        }
        return mock_time;
    }

    struct tm *localtime_r(const time_t *timep, struct tm *result) {
        (void)timep;  // Unused
        memcpy(result, &mock_timeinfo, sizeof(struct tm));
        return result;
    }
}

// Test fixture
class CalculateNextAlignedWakeupTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Reset mocks before each test
        mock_sleep_schedule_enabled = false;
        mock_sleep_start = 1380;  // 23:00
        mock_sleep_end = 420;     // 07:00
        SetMockTime(0, 0, 0);
    }

    void SetMockTime(int hour, int minute, int second) {
        memset(&mock_timeinfo, 0, sizeof(mock_timeinfo));
        mock_timeinfo.tm_hour = hour;
        mock_timeinfo.tm_min = minute;
        mock_timeinfo.tm_sec = second;
        mock_timeinfo.tm_year = 126;  // 2026
        mock_timeinfo.tm_mon = 0;     // January
        mock_timeinfo.tm_mday = 20;
    }
};

// Test Case 1: No sleep schedule - simple clock alignment
TEST_F(CalculateNextAlignedWakeupTest, NoSleepSchedule1HourInterval) {
    mock_sleep_schedule_enabled = false;
    SetMockTime(10, 30, 0);
    
    int result = calculate_next_aligned_wakeup(3600);
    
    EXPECT_EQ(1800, result) << "Should wake in 30 minutes (at 11:00)";
}

// Test Case 2: No sleep schedule - 30 minute interval
TEST_F(CalculateNextAlignedWakeupTest, NoSleepSchedule30MinInterval) {
    mock_sleep_schedule_enabled = false;
    SetMockTime(10, 15, 0);
    
    int result = calculate_next_aligned_wakeup(1800);
    
    EXPECT_EQ(900, result) << "Should wake in 15 minutes (at 10:30)";
}

// Test Case 3: Sleep schedule enabled, next wake-up is outside schedule
TEST_F(CalculateNextAlignedWakeupTest, SleepScheduleWakeOutside) {
    mock_sleep_schedule_enabled = true;
    mock_sleep_start = 1380;  // 23:00
    mock_sleep_end = 420;     // 07:00
    SetMockTime(18, 0, 0);
    
    int result = calculate_next_aligned_wakeup(3600);
    
    EXPECT_EQ(3600, result) << "Should wake in 1 hour (at 19:00)";
}

// Test Case 4: Sleep schedule enabled, next wake-up would be inside schedule
TEST_F(CalculateNextAlignedWakeupTest, SleepScheduleWakeInside) {
    mock_sleep_schedule_enabled = true;
    mock_sleep_start = 1380;  // 23:00
    mock_sleep_end = 420;     // 07:00
    SetMockTime(22, 30, 0);
    
    int result = calculate_next_aligned_wakeup(3600);
    
    EXPECT_EQ(30600, result) << "Should skip to 07:00 next day (8.5 hours) - sleep_end is exclusive";
}

// Test Case 5: Currently in sleep schedule
TEST_F(CalculateNextAlignedWakeupTest, CurrentlyInSleepSchedule) {
    mock_sleep_schedule_enabled = true;
    mock_sleep_start = 1380;  // 23:00
    mock_sleep_end = 420;     // 07:00
    SetMockTime(2, 0, 0);
    
    int result = calculate_next_aligned_wakeup(3600);
    
    EXPECT_EQ(18000, result) << "Should wake at 07:00 (5 hours) - sleep_end is exclusive";
}

// Test Case 6: Sleep schedule ends at aligned time
TEST_F(CalculateNextAlignedWakeupTest, SleepScheduleEndsAtAlignedTime) {
    mock_sleep_schedule_enabled = true;
    mock_sleep_start = 1380;  // 23:00
    mock_sleep_end = 420;     // 07:00
    SetMockTime(6, 0, 0);
    
    int result = calculate_next_aligned_wakeup(3600);
    
    EXPECT_EQ(3600, result) << "Should wake at 07:00 (1 hour)";
}

// Test Case 7: Sleep schedule with 2-hour interval
TEST_F(CalculateNextAlignedWakeupTest, SleepSchedule2HourInterval) {
    mock_sleep_schedule_enabled = true;
    mock_sleep_start = 1380;  // 23:00
    mock_sleep_end = 435;     // 07:15
    SetMockTime(22, 0, 0);
    
    int result = calculate_next_aligned_wakeup(7200);
    
    EXPECT_EQ(36000, result) << "Should skip to 08:00 next day (10 hours) - first aligned time >= sleep_end";
}

// Test Case 8: Same-day schedule (not overnight)
TEST_F(CalculateNextAlignedWakeupTest, SameDaySchedule) {
    mock_sleep_schedule_enabled = true;
    mock_sleep_start = 720;   // 12:00
    mock_sleep_end = 840;     // 14:00
    SetMockTime(11, 30, 0);
    
    int result = calculate_next_aligned_wakeup(3600);
    
    EXPECT_EQ(9000, result) << "Should skip to 14:00 (2.5 hours) - sleep_end is exclusive";
}

// Test Case 9: Edge case - exactly at midnight
TEST_F(CalculateNextAlignedWakeupTest, ExactlyAtMidnight) {
    mock_sleep_schedule_enabled = true;
    mock_sleep_start = 1380;  // 23:00
    mock_sleep_end = 420;     // 07:00
    SetMockTime(0, 0, 0);
    
    int result = calculate_next_aligned_wakeup(3600);
    
    EXPECT_EQ(25200, result) << "Should wake at 07:00 (7 hours) - sleep_end is exclusive";
}

// Test Case 10: 15-minute interval
TEST_F(CalculateNextAlignedWakeupTest, FifteenMinuteInterval) {
    mock_sleep_schedule_enabled = false;
    SetMockTime(10, 7, 0);
    
    int result = calculate_next_aligned_wakeup(900);
    
    EXPECT_EQ(480, result) << "Should wake at 10:15 (8 minutes)";
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
