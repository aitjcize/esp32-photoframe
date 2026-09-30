#include <gtest/gtest.h>

#include <vector>

#include "battery_adc.h"
#include "board_hal.h"
#include "driver/gpio.h"
#include "esp_adc/adc_cali_scheme.h"

namespace
{
int reads, raw_value, failed_reads, pin_level, deleted_units;
esp_err_t init_result, channel_result, cali_result, conversion_result;
std::vector<unsigned> pin_levels;
}  // namespace

extern "C" {
const char *esp_err_to_name(esp_err_t)
{
    return "test error";
}
esp_err_t gpio_set_level(gpio_num_t, unsigned level)
{
    pin_level = level;
    pin_levels.push_back(level);
    return ESP_OK;
}
esp_err_t adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t *, adc_oneshot_unit_handle_t *out)
{
    *out = reinterpret_cast<void *>(1);
    return init_result;
}
esp_err_t adc_oneshot_config_channel(adc_oneshot_unit_handle_t, adc_channel_t,
                                     const adc_oneshot_chan_cfg_t *)
{
    return channel_result;
}
esp_err_t adc_oneshot_read(adc_oneshot_unit_handle_t, adc_channel_t, int *out)
{
    EXPECT_EQ(pin_level, 1);
    ++reads;
    *out = raw_value;
    return reads <= failed_reads ? ESP_FAIL : ESP_OK;
}
esp_err_t adc_oneshot_del_unit(adc_oneshot_unit_handle_t)
{
    ++deleted_units;
    return ESP_OK;
}
esp_err_t adc_cali_create_scheme_curve_fitting(const adc_cali_curve_fitting_config_t *,
                                               adc_cali_handle_t *out)
{
    *out = reinterpret_cast<void *>(2);
    return cali_result;
}
esp_err_t adc_cali_delete_scheme_curve_fitting(adc_cali_handle_t)
{
    return ESP_OK;
}
esp_err_t adc_cali_raw_to_voltage(adc_cali_handle_t, int raw, int *out)
{
    *out = raw;  // Test calibration maps raw counts to mV directly.
    return conversion_result;
}
}

class BatteryAdc : public testing::Test
{
   protected:
    battery_adc_t *adc = nullptr;
    battery_adc_config_t cfg = {ADC_UNIT_1, ADC_CHANNEL_0, ADC_ATTEN_DB_12, 6, 10, 8, 2.0f,
                                1.0f,       true};
    void SetUp() override
    {
        board_hal_use_boot_battery_sample(false);
        reads = failed_reads = pin_level = deleted_units = 0;
        raw_value = 2000;
        init_result = channel_result = cali_result = conversion_result = ESP_OK;
        pin_levels.clear();
    }
    void TearDown() override
    {
        battery_adc_destroy(adc);
    }
    void Create()
    {
        ASSERT_EQ(battery_adc_create(&cfg, &adc), ESP_OK);
    }
};

TEST_F(BatteryAdc, ScheduledTelemetryKeepsEarlyReadingAfterLoadChanges)
{
    Create();
    EXPECT_EQ(reads, 8);
    EXPECT_EQ(pin_levels, (std::vector<unsigned>{1, 0}));
    board_hal_use_boot_battery_sample(true);
    raw_value = 1800;
    for (int i = 0; i < 4; ++i)
        EXPECT_EQ(battery_adc_read_mv(adc), 4000);
    EXPECT_EQ(reads, 8);
    EXPECT_EQ(pin_level, 0);
}

TEST_F(BatteryAdc, InteractiveReadsStayFresh)
{
    Create();
    raw_value = 1800;
    EXPECT_EQ(battery_adc_read_mv(adc), 3600);
    raw_value = 2100;
    EXPECT_EQ(battery_adc_read_mv(adc), 4200);
    EXPECT_EQ(reads, 24);
}

TEST_F(BatteryAdc, SwitchingBackToLiveReadsRefreshes)
{
    Create();
    board_hal_use_boot_battery_sample(true);
    raw_value = 1900;
    EXPECT_EQ(battery_adc_read_mv(adc), 4000);
    board_hal_use_boot_battery_sample(false);
    EXPECT_EQ(battery_adc_read_mv(adc), 3800);
}

TEST_F(BatteryAdc, OtherBoardsRemainLiveEvenInScheduledMode)
{
    cfg.sample_at_create = false;
    Create();
    EXPECT_EQ(reads, 0);
    board_hal_use_boot_battery_sample(true);
    EXPECT_EQ(battery_adc_read_mv(adc), 4000);
    raw_value = 1900;
    EXPECT_EQ(battery_adc_read_mv(adc), 3800);
}

TEST_F(BatteryAdc, FailedEarlyReadingStaysUnknownAndDisablesDivider)
{
    failed_reads = 8;
    Create();
    EXPECT_EQ(pin_levels, (std::vector<unsigned>{1, 0}));
    board_hal_use_boot_battery_sample(true);
    EXPECT_EQ(battery_adc_read_mv(adc), -1);
    EXPECT_EQ(reads, 8);
    board_hal_use_boot_battery_sample(false);
    EXPECT_EQ(battery_adc_read_mv(adc), 4000);
}

TEST_F(BatteryAdc, PartialSampleFailuresStillUseSuccessfulSamples)
{
    failed_reads = 4;
    Create();
    board_hal_use_boot_battery_sample(true);
    EXPECT_EQ(battery_adc_read_mv(adc), 4000);
    EXPECT_EQ(pin_level, 0);
}

TEST_F(BatteryAdc, InitFailureRemainsUnknown)
{
    init_result = ESP_FAIL;
    EXPECT_EQ(battery_adc_create(&cfg, &adc), ESP_FAIL);
    board_hal_use_boot_battery_sample(true);
    EXPECT_EQ(battery_adc_read_mv(adc), -1);
    EXPECT_EQ(reads, 0);
    EXPECT_EQ(pin_level, 0);
}

TEST_F(BatteryAdc, ChannelFailureCleansUp)
{
    channel_result = ESP_FAIL;
    EXPECT_EQ(battery_adc_create(&cfg, &adc), ESP_FAIL);
    EXPECT_EQ(deleted_units, 1);
    EXPECT_EQ(reads, 0);
    EXPECT_EQ(battery_adc_read_mv(adc), -1);
}

TEST_F(BatteryAdc, CalibrationUnavailableKeepsLinearFallback)
{
    cali_result = ESP_ERR_NOT_SUPPORTED;
    Create();
    board_hal_use_boot_battery_sample(true);
    EXPECT_EQ(battery_adc_read_mv(adc), static_cast<int>(2000.0f * (3300.0f / 4095.0f) * 2.0f));
}

TEST_F(BatteryAdc, ConversionFailureKeepsLinearFallbackAndDisablesDivider)
{
    conversion_result = ESP_FAIL;
    Create();
    board_hal_use_boot_battery_sample(true);
    EXPECT_EQ(battery_adc_read_mv(adc), static_cast<int>(2000.0f * (3300.0f / 4095.0f) * 2.0f));
    EXPECT_EQ(pin_level, 0);
}
