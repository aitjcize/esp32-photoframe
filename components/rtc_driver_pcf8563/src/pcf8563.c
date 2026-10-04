#include "pcf8563.h"

#include <string.h>
#include <time.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "pcf8563_rtc";

// PCF8563T I2C address
#define PCF8563_I2C_ADDR 0x51

// Register addresses
#define PCF8563_REG_CONTROL_1 0x00
#define PCF8563_REG_CONTROL_2 0x01
#define PCF8563_REG_VL_SECONDS 0x02  // bit7 = VL (voltage low flag)
#define PCF8563_REG_MINUTES 0x03
#define PCF8563_REG_HOURS 0x04
#define PCF8563_REG_DAYS 0x05
#define PCF8563_REG_WEEKDAYS 0x06
#define PCF8563_REG_MONTHS 0x07  // bit7 = century (0=2000s, 1=1900s)
#define PCF8563_REG_YEARS 0x08

// Bit masks
#define PCF8563_STOP_BIT 0x20     // STOP bit in Control_1
#define PCF8563_VL_BIT 0x80       // Voltage Low flag in VL_Seconds register
#define PCF8563_CENTURY_BIT 0x80  // Century bit in Months register

static bool rtc_initialized = false;
static bool rtc_available = false;
static i2c_master_dev_handle_t rtc_dev_handle = NULL;

static uint8_t bcd_to_dec(uint8_t bcd)
{
    return (bcd >> 4) * 10 + (bcd & 0x0F);
}

static uint8_t dec_to_bcd(uint8_t dec)
{
    return ((dec / 10) << 4) | (dec % 10);
}

esp_err_t pcf8563_init(i2c_master_bus_handle_t i2c_bus)
{
    ESP_LOGI(TAG, "Initializing PCF8563T RTC");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PCF8563_I2C_ADDR,
        .scl_speed_hz = 100000,
    };

    esp_err_t ret = i2c_master_bus_add_device(i2c_bus, &dev_cfg, &rtc_dev_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add PCF8563T device: %s", esp_err_to_name(ret));
        return ret;
    }

    // Read Control_1 to verify device presence
    uint8_t ctrl1;
    ret = i2c_master_transmit_receive(rtc_dev_handle, (uint8_t[]){PCF8563_REG_CONTROL_1}, 1, &ctrl1,
                                      1, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to communicate with PCF8563T: %s", esp_err_to_name(ret));
        rtc_available = false;
        rtc_initialized = true;
        return ret;
    }

    // Clear STOP bit to ensure the clock is running
    ctrl1 &= ~PCF8563_STOP_BIT;
    uint8_t write_buf[2] = {PCF8563_REG_CONTROL_1, ctrl1};
    ret = i2c_master_transmit(rtc_dev_handle, write_buf, 2, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure PCF8563T: %s", esp_err_to_name(ret));
        rtc_available = false;
        rtc_initialized = true;
        return ret;
    }

    ESP_LOGI(TAG, "PCF8563T RTC initialized successfully");
    rtc_available = true;
    rtc_initialized = true;
    return ESP_OK;
}

esp_err_t pcf8563_read_time(time_t *time_out)
{
    if (!rtc_initialized) {
        ESP_LOGE(TAG, "PCF8563T not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (!rtc_available) {
        ESP_LOGD(TAG, "PCF8563T not available");
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t data[7];
    esp_err_t ret = i2c_master_transmit_receive(rtc_dev_handle, (uint8_t[]){PCF8563_REG_VL_SECONDS},
                                                1, data, 7, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read time from PCF8563T: %s", esp_err_to_name(ret));
        return ret;
    }

    // Check voltage low flag (time may be invalid after power loss)
    if (data[0] & PCF8563_VL_BIT) {
        ESP_LOGW(TAG, "PCF8563T VL flag set - time may be invalid after power loss");
        return ESP_ERR_INVALID_STATE;
    }

    struct tm timeinfo = {0};
    timeinfo.tm_sec = bcd_to_dec(data[0] & 0x7F);
    timeinfo.tm_min = bcd_to_dec(data[1] & 0x7F);
    timeinfo.tm_hour = bcd_to_dec(data[2] & 0x3F);
    timeinfo.tm_mday = bcd_to_dec(data[3] & 0x3F);
    timeinfo.tm_wday = bcd_to_dec(data[4] & 0x07);
    timeinfo.tm_mon = bcd_to_dec(data[5] & 0x1F) - 1;  // tm_mon is 0-11

    // Century bit: 0 = 2000s, 1 = 1900s
    int century = (data[5] & PCF8563_CENTURY_BIT) ? 1900 : 2000;
    timeinfo.tm_year = bcd_to_dec(data[6]) + century - 1900;  // years since 1900
    // See the identical fix/comment in rtc_driver_pcf85063's read function - same {0}-init-
    // forces-standard-time bug, same one-hour-during-DST symptom.
    timeinfo.tm_isdst = -1;

    *time_out = mktime(&timeinfo);

    ESP_LOGI(TAG, "Read time from PCF8563T: %04d-%02d-%02d %02d:%02d:%02d", timeinfo.tm_year + 1900,
             timeinfo.tm_mon + 1, timeinfo.tm_mday, timeinfo.tm_hour, timeinfo.tm_min,
             timeinfo.tm_sec);

    return ESP_OK;
}

esp_err_t pcf8563_write_time(time_t time_in)
{
    if (!rtc_initialized) {
        ESP_LOGE(TAG, "PCF8563T not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (!rtc_available) {
        ESP_LOGD(TAG, "PCF8563T not available");
        return ESP_ERR_NOT_FOUND;
    }

    struct tm timeinfo;
    localtime_r(&time_in, &timeinfo);

    int year_full = timeinfo.tm_year + 1900;
    uint8_t century_bit = (year_full >= 2000) ? 0 : PCF8563_CENTURY_BIT;
    int year_2digit = year_full - ((year_full >= 2000) ? 2000 : 1900);

    uint8_t data[7];
    data[0] = dec_to_bcd(timeinfo.tm_sec) & 0x7F;  // Clear VL bit
    data[1] = dec_to_bcd(timeinfo.tm_min) & 0x7F;
    data[2] = dec_to_bcd(timeinfo.tm_hour) & 0x3F;
    data[3] = dec_to_bcd(timeinfo.tm_mday) & 0x3F;
    data[4] = dec_to_bcd(timeinfo.tm_wday) & 0x07;
    data[5] = century_bit | (dec_to_bcd(timeinfo.tm_mon + 1) & 0x1F);
    data[6] = dec_to_bcd(year_2digit);

    uint8_t write_buf[8];
    write_buf[0] = PCF8563_REG_VL_SECONDS;
    memcpy(&write_buf[1], data, 7);

    esp_err_t ret = i2c_master_transmit(rtc_dev_handle, write_buf, 8, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write time to PCF8563T: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Wrote time to PCF8563T: %04d-%02d-%02d %02d:%02d:%02d", year_full,
             timeinfo.tm_mon + 1, timeinfo.tm_mday, timeinfo.tm_hour, timeinfo.tm_min,
             timeinfo.tm_sec);

    return ESP_OK;
}

bool pcf8563_is_available(void)
{
    return rtc_available;
}
