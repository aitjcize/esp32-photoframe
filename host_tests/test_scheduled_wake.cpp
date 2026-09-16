#include <gtest/gtest.h>

#include <csetjmp>

extern "C" {
#include "freertos/task.h"
#include "scheduled_wake.h"
}

namespace
{
TaskFunction_t pending_task;
void *pending_arg;
int create_calls;
int pipeline_calls;
int sleep_calls;
BaseType_t create_result;
wakeup_source_t pipeline_source;
bool pipeline_returns;
bool sleep_returns;
std::jmp_buf sleep_jump;

class ScheduledWake : public testing::Test
{
   protected:
    void SetUp() override
    {
        pending_task = nullptr;
        pending_arg = nullptr;
        create_calls = pipeline_calls = sleep_calls = 0;
        create_result = pdPASS;
        pipeline_source = WAKEUP_SOURCE_NONE;
        pipeline_returns = sleep_returns = false;
    }
};
}  // namespace

extern "C" BaseType_t xTaskCreate(TaskFunction_t task, const char *name, uint32_t stack_bytes,
                                  void *arg, UBaseType_t priority, TaskHandle_t *handle)
{
    ++create_calls;
    EXPECT_STREQ(name, "deep_sleep_wake");
    EXPECT_EQ(stack_bytes, 12288u);
    EXPECT_EQ(priority, 5u);
    EXPECT_EQ(handle, nullptr);
    if (create_result == pdPASS) {
        pending_task = task;
        pending_arg = arg;
    }
    return create_result;
}

extern "C" void power_manager_enter_sleep(void)
{
    ++sleep_calls;
    if (!sleep_returns) {
        std::longjmp(sleep_jump, 1);
    }
}

extern "C" void deep_sleep_wake_main(wakeup_source_t source)
{
    ++pipeline_calls;
    pipeline_source = source;
    if (!pipeline_returns) {
        power_manager_enter_sleep();
    }
}

TEST_F(ScheduledWake, InteractiveAndClearWakesStayOnExistingPaths)
{
    for (auto source : {WAKEUP_SOURCE_NONE, WAKEUP_SOURCE_BOOT_BUTTON, WAKEUP_SOURCE_CLEAR_BUTTON,
                        WAKEUP_SOURCE_EXT1_UNKNOWN}) {
        EXPECT_FALSE(scheduled_wake_start(source));
    }
    EXPECT_EQ(create_calls, 0);
    EXPECT_EQ(pipeline_calls, 0);
    EXPECT_EQ(sleep_calls, 0);
}

TEST_F(ScheduledWake, TimerWakeRunsOnlyAfterHandoff)
{
    ASSERT_TRUE(scheduled_wake_start(WAKEUP_SOURCE_TIMER));
    ASSERT_NE(pending_task, nullptr);
    EXPECT_EQ(create_calls, 1);
    EXPECT_EQ(pipeline_calls, 0);
    EXPECT_EQ(sleep_calls, 0);

    if (setjmp(sleep_jump) == 0) {
        pending_task(pending_arg);
        FAIL() << "Wake task returned without sleeping";
    }
    EXPECT_EQ(pipeline_calls, 1);
    EXPECT_EQ(pipeline_source, WAKEUP_SOURCE_TIMER);
    EXPECT_EQ(sleep_calls, 1);
}

TEST_F(ScheduledWake, RotateWakeSourceSurvivesCallerReturning)
{
    {
        wakeup_source_t source = WAKEUP_SOURCE_ROTATE_BUTTON;
        ASSERT_TRUE(scheduled_wake_start(source));
        source = WAKEUP_SOURCE_NONE;
    }
    ASSERT_NE(pending_task, nullptr);
    if (setjmp(sleep_jump) == 0) {
        pending_task(pending_arg);
        FAIL() << "Wake task returned without sleeping";
    }
    EXPECT_EQ(pipeline_calls, 1);
    EXPECT_EQ(pipeline_source, WAKEUP_SOURCE_ROTATE_BUTTON);
    EXPECT_EQ(sleep_calls, 1);
}

TEST_F(ScheduledWake, AllocationFailureSleepsWithoutRunningPipeline)
{
    create_result = pdFALSE;
    if (setjmp(sleep_jump) == 0) {
        scheduled_wake_start(WAKEUP_SOURCE_TIMER);
        FAIL() << "Allocation failure returned to boot initialization";
    }
    EXPECT_EQ(create_calls, 1);
    EXPECT_EQ(pending_task, nullptr);
    EXPECT_EQ(pipeline_calls, 0);
    EXPECT_EQ(sleep_calls, 1);
}

TEST_F(ScheduledWake, UnexpectedPipelineReturnStillSleeps)
{
    pipeline_returns = true;
    ASSERT_TRUE(scheduled_wake_start(WAKEUP_SOURCE_TIMER));
    if (setjmp(sleep_jump) == 0) {
        pending_task(pending_arg);
        FAIL() << "Wake task returned without sleeping";
    }
    EXPECT_EQ(pipeline_calls, 1);
    EXPECT_EQ(sleep_calls, 1);
}

TEST_F(ScheduledWake, ReturningSleepCannotFallThroughAfterAllocationFailure)
{
    create_result = pdFALSE;
    sleep_returns = true;
    EXPECT_DEATH(scheduled_wake_start(WAKEUP_SOURCE_TIMER), "Could not allocate wake task");
}

TEST_F(ScheduledWake, ReturningSleepCannotReturnFromTask)
{
    pipeline_returns = sleep_returns = true;
    ASSERT_TRUE(scheduled_wake_start(WAKEUP_SOURCE_TIMER));
    EXPECT_DEATH(pending_task(pending_arg), "Wake pipeline returned unexpectedly");
}
