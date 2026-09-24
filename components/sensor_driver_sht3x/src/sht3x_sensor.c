// SHT3x (SHT30/31/35) temperature + humidity driver.
//
// Same I2C address (0x44) and CRC-8 as the SHT4x, but a different command set:
// SHT3x commands are 16-bit and there is no "measure" opcode shared with the
// SHT4x, so this cannot reuse sensor_driver_sht40. Used by the M5Paper, which
// fits an SHT30 next to the BM8563 RTC on I2C0.

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sensor.h"

static const char *TAG = "sht3x_sensor";

// SHT3x I2C address (ADDR pin tied low, as on the M5Paper)
#define SHT3X_I2C_ADDR 0x44

// SHT3x commands (16-bit, MSB first)
#define SHT3X_CMD_SOFT_RESET 0x30A2
// Single shot, high repeatability, clock stretching disabled (~15ms)
#define SHT3X_CMD_MEASURE_HIGH_PRECISION 0x2400
// Stop periodic measurement / return to idle ("break")
#define SHT3X_CMD_BREAK 0x3093
#define SHT3X_CMD_HEATER_DISABLE 0x3066

static i2c_master_dev_handle_t sht3x_dev_handle = NULL;
static bool sensor_initialized = false;
static bool sensor_available = false;

// CRC-8 calculation for SHT3x (polynomial: 0x31, init: 0xFF) — identical to SHT4x
static uint8_t calculate_crc(const uint8_t *data, size_t length)
{
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (uint8_t bit = 8; bit > 0; --bit) {
            if (crc & 0x80) {
                crc = (crc << 1) ^ 0x31;
            } else {
                crc = (crc << 1);
            }
        }
    }
    return crc;
}

static esp_err_t sht3x_send_cmd(uint16_t cmd)
{
    uint8_t buf[2] = {(uint8_t) (cmd >> 8), (uint8_t) (cmd & 0xFF)};
    return i2c_master_transmit(sht3x_dev_handle, buf, sizeof(buf), pdMS_TO_TICKS(20));
}

esp_err_t sensor_init(i2c_master_bus_handle_t i2c_bus)
{
    ESP_LOGI(TAG, "Initializing SHT3x sensor");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SHT3X_I2C_ADDR,
        .scl_speed_hz = 400000,  // SHT3x supports up to 1MHz
    };

    esp_err_t ret = i2c_master_bus_add_device(i2c_bus, &dev_cfg, &sht3x_dev_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add SHT3x device: %s", esp_err_to_name(ret));
        sensor_available = false;
        sensor_initialized = true;
        return ret;
    }

    ret = sht3x_send_cmd(SHT3X_CMD_SOFT_RESET);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to reset SHT3x: %s", esp_err_to_name(ret));
        sensor_available = false;
        sensor_initialized = true;
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(2));  // Soft reset takes up to 1.5ms

    // The heater would bias the reading we feed to the IT8951 waveform picker.
    // It is off after reset; make that explicit and ignore a failure.
    sht3x_send_cmd(SHT3X_CMD_HEATER_DISABLE);

    ESP_LOGI(TAG, "SHT3x sensor initialized successfully");
    sensor_available = true;
    sensor_initialized = true;
    return ESP_OK;
}

esp_err_t sensor_read(float *temperature, float *humidity)
{
    if (!sensor_initialized) {
        ESP_LOGE(TAG, "SHT3x not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (!sensor_available) {
        ESP_LOGD(TAG, "SHT3x sensor not available");
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t data[6];  // T_MSB, T_LSB, T_CRC, RH_MSB, RH_LSB, RH_CRC

    esp_err_t ret = sht3x_send_cmd(SHT3X_CMD_MEASURE_HIGH_PRECISION);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to trigger SHT3x measurement: %s", esp_err_to_name(ret));
        return ret;
    }

    // Wait for measurement (high repeatability: max 15ms)
    vTaskDelay(pdMS_TO_TICKS(20));

    ret = i2c_master_receive(sht3x_dev_handle, data, sizeof(data), pdMS_TO_TICKS(20));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read SHT3x data: %s", esp_err_to_name(ret));
        return ret;
    }

    uint8_t temp_crc = calculate_crc(data, 2);
    if (temp_crc != data[2]) {
        ESP_LOGE(TAG, "Temperature CRC mismatch: expected 0x%02X, got 0x%02X", temp_crc, data[2]);
        return ESP_ERR_INVALID_CRC;
    }

    uint8_t hum_crc = calculate_crc(&data[3], 2);
    if (hum_crc != data[5]) {
        ESP_LOGE(TAG, "Humidity CRC mismatch: expected 0x%02X, got 0x%02X", hum_crc, data[5]);
        return ESP_ERR_INVALID_CRC;
    }

    uint16_t temp_raw = (data[0] << 8) | data[1];
    uint16_t hum_raw = (data[3] << 8) | data[4];

    // Temperature conversion: T = -45 + 175 * (raw / 65535)
    *temperature = -45.0f + 175.0f * ((float) temp_raw / 65535.0f);

    // Humidity conversion: RH = 100 * (raw / 65535)
    *humidity = 100.0f * ((float) hum_raw / 65535.0f);

    ESP_LOGD(TAG, "Temperature: %.2f°C, Humidity: %.2f%%", *temperature, *humidity);

    return ESP_OK;
}

bool sensor_is_available(void)
{
    return sensor_available;
}

esp_err_t sensor_sleep(void)
{
    if (!sensor_available)
        return ESP_OK;
    // Single-shot mode already returns to idle (~0.2 µA) after each conversion.
    // Issue a break anyway so a stray periodic-mode command can't leave the
    // sensor converting across deep sleep.
    sht3x_send_cmd(SHT3X_CMD_BREAK);
    return ESP_OK;
}

esp_err_t sensor_wakeup(void)
{
    // SHT3x idles between single-shot measurements; no wake command needed.
    return ESP_OK;
}
