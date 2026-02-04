#include "pcf85063_rtc.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "pcf85063_rtc";

// PCF85063ATL Register addresses
#define PCF85063_ADDR_CONTROL_1 0x00
#define PCF85063_ADDR_CONTROL_2 0x01
#define PCF85063_ADDR_OFFSET 0x02
#define PCF85063_ADDR_RAM_BYTE 0x03
#define PCF85063_ADDR_SECONDS 0x04
#define PCF85063_ADDR_MINUTES 0x05
#define PCF85063_ADDR_HOURS 0x06
#define PCF85063_ADDR_DAYS 0x07
#define PCF85063_ADDR_WEEKDAYS 0x08
#define PCF85063_ADDR_MONTHS 0x09
#define PCF85063_ADDR_YEARS 0x0A

// Bit masks
#define PCF85063_STOP_BIT 0x20     // STOP bit in Control_1 register
#define PCF85063_CAP_SEL_BIT 0x01  // Capacitor selection bit
#define PCF85063_OSF_BIT 0x80      // Oscillator stop flag (in seconds register)

static bool rtc_initialized = false;
static bool rtc_available = false;
static i2c_master_dev_handle_t rtc_dev_handle = NULL;

// BCD conversion helpers
static uint8_t bcd_to_dec(uint8_t bcd)
{
    return (bcd >> 4) * 10 + (bcd & 0x0F);
}

static uint8_t dec_to_bcd(uint8_t dec)
{
    return ((dec / 10) << 4) | (dec % 10);
}

esp_err_t pcf85063_init(i2c_master_bus_handle_t i2c_bus)
{
    esp_err_t ret;
    uint8_t data;

    ESP_LOGI(TAG, "Initializing PCF85063ATL RTC");

    // Create device handle
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x51,  // PCF85063 address
        .scl_speed_hz = 100000,
    };

    ret = i2c_master_bus_add_device(i2c_bus, &dev_cfg, &rtc_dev_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add PCF85063 device: %s", esp_err_to_name(ret));
        return ret;
    }

    // Try to read control register to verify device presence
    ret = i2c_master_transmit_receive(rtc_dev_handle, (uint8_t[]){PCF85063_ADDR_CONTROL_1}, 1,
                                      &data, 1, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to communicate with PCF85063ATL: %s", esp_err_to_name(ret));
        rtc_available = false;
        rtc_initialized = true;
        return ret;
    }

    // Clear STOP bit if set (enable counting) and set CAP_SEL for 7pF (typical for PCF85063ATL)
    data &= ~PCF85063_STOP_BIT;    // Clear STOP bit
    data |= PCF85063_CAP_SEL_BIT;  // Set CAP_SEL for 7pF load capacitance
    uint8_t write_buf[2] = {PCF85063_ADDR_CONTROL_1, data};
    ret = i2c_master_transmit(rtc_dev_handle, write_buf, 2, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure PCF85063ATL: %s", esp_err_to_name(ret));
        rtc_available = false;
        rtc_initialized = true;
        return ret;
    }

    ESP_LOGI(TAG, "PCF85063ATL RTC initialized successfully");
    rtc_available = true;
    rtc_initialized = true;
    return ESP_OK;
}

esp_err_t pcf85063_read_time(time_t *time_out)
{
    if (!rtc_initialized) {
        ESP_LOGE(TAG, "PCF85063ATL not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (!rtc_available) {
        ESP_LOGD(TAG, "PCF85063ATL not available");
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t data[7];
    esp_err_t ret;

    // Read time registers (seconds through years)
    ret = i2c_master_transmit_receive(rtc_dev_handle, (uint8_t[]){PCF85063_ADDR_SECONDS}, 1, data,
                                      7, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read time from PCF85063ATL: %s", esp_err_to_name(ret));
        return ret;
    }

    // Check oscillator stop flag
    if (data[0] & PCF85063_OSF_BIT) {
        ESP_LOGW(TAG, "PCF85063ATL oscillator was stopped - time may be invalid");
        return ESP_ERR_INVALID_STATE;
    }

    // Convert BCD to decimal
    struct tm timeinfo = {0};
    timeinfo.tm_sec = bcd_to_dec(data[0] & 0x7F);
    timeinfo.tm_min = bcd_to_dec(data[1] & 0x7F);
    timeinfo.tm_hour = bcd_to_dec(data[2] & 0x3F);
    timeinfo.tm_mday = bcd_to_dec(data[3] & 0x3F);
    timeinfo.tm_wday = bcd_to_dec(data[4] & 0x07);
    timeinfo.tm_mon = bcd_to_dec(data[5] & 0x1F) - 1;  // tm_mon is 0-11
    timeinfo.tm_year = bcd_to_dec(data[6]) + 100;      // Years since 1900, RTC starts at 2000

    *time_out = mktime(&timeinfo);

    ESP_LOGI(TAG, "Read time from PCF85063ATL: %04d-%02d-%02d %02d:%02d:%02d",
             timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday, timeinfo.tm_hour,
             timeinfo.tm_min, timeinfo.tm_sec);

    return ESP_OK;
}

esp_err_t pcf85063_write_time(time_t time_in)
{
    if (!rtc_initialized) {
        ESP_LOGE(TAG, "PCF85063ATL not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (!rtc_available) {
        ESP_LOGD(TAG, "PCF85063ATL not available");
        return ESP_ERR_NOT_FOUND;
    }

    struct tm timeinfo;
    localtime_r(&time_in, &timeinfo);

    uint8_t data[7];

    // Convert to BCD and prepare data
    data[0] = dec_to_bcd(timeinfo.tm_sec) & 0x7F;  // Clear OSF bit
    data[1] = dec_to_bcd(timeinfo.tm_min) & 0x7F;
    data[2] = dec_to_bcd(timeinfo.tm_hour) & 0x3F;
    data[3] = dec_to_bcd(timeinfo.tm_mday) & 0x3F;
    data[4] = dec_to_bcd(timeinfo.tm_wday) & 0x07;
    data[5] = dec_to_bcd(timeinfo.tm_mon + 1) & 0x1F;  // Month (1-12), no century bit

    // Year (0-99, assumes 2000-2099)
    int year = timeinfo.tm_year + 1900;
    data[6] = dec_to_bcd(year - 2000);

    uint8_t write_buf[8];
    write_buf[0] = PCF85063_ADDR_SECONDS;
    memcpy(&write_buf[1], data, 7);

    esp_err_t ret = i2c_master_transmit(rtc_dev_handle, write_buf, 8, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write time to PCF85063ATL: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Wrote time to PCF85063ATL: %04d-%02d-%02d %02d:%02d:%02d", year,
             timeinfo.tm_mon + 1, timeinfo.tm_mday, timeinfo.tm_hour, timeinfo.tm_min,
             timeinfo.tm_sec);

    return ESP_OK;
}

bool pcf85063_is_available(void)
{
    return rtc_available;
}
