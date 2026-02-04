#include "sht40_sensor.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sht40_sensor";

// SHT40 I2C address (fixed for SHT40-AD1B-R2)
#define SHT40_I2C_ADDR 0x44

// SHT40 Commands
#define SHT40_CMD_MEASURE_HIGH_PRECISION 0xFD
#define SHT40_CMD_SOFT_RESET 0x94
#define SHT40_CMD_READ_SERIAL 0x89

static i2c_master_dev_handle_t sht40_dev_handle = NULL;
static bool sensor_initialized = false;
static bool sensor_available = false;

// CRC-8 calculation for SHT40 (polynomial: 0x31, init: 0xFF)
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

esp_err_t sht40_init(i2c_master_bus_handle_t i2c_bus)
{
    ESP_LOGI(TAG, "Initializing SHT40 sensor");

    // Create device handle
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SHT40_I2C_ADDR,
        .scl_speed_hz = 400000,  // SHT40 supports up to 1MHz
    };

    esp_err_t ret = i2c_master_bus_add_device(i2c_bus, &dev_cfg, &sht40_dev_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add SHT40 device: %s", esp_err_to_name(ret));
        sensor_available = false;
        sensor_initialized = true;
        return ret;
    }

    // Soft reset
    uint8_t cmd = SHT40_CMD_SOFT_RESET;
    ret = i2c_master_transmit(sht40_dev_handle, &cmd, 1, pdMS_TO_TICKS(10));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to reset SHT40: %s", esp_err_to_name(ret));
        sensor_available = false;
        sensor_initialized = true;
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(1));  // Wait for reset

    ESP_LOGI(TAG, "SHT40 sensor initialized successfully");
    sensor_available = true;
    sensor_initialized = true;
    return ESP_OK;
}

esp_err_t sht40_read(float *temperature, float *humidity)
{
    if (!sensor_initialized) {
        ESP_LOGE(TAG, "SHT40 not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (!sensor_available) {
        ESP_LOGD(TAG, "SHT40 sensor not available");
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t cmd = SHT40_CMD_MEASURE_HIGH_PRECISION;
    uint8_t data[6];  // T_MSB, T_LSB, T_CRC, RH_MSB, RH_LSB, RH_CRC

    // Trigger measurement
    esp_err_t ret = i2c_master_transmit(sht40_dev_handle, &cmd, 1, pdMS_TO_TICKS(10));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to trigger SHT40 measurement: %s", esp_err_to_name(ret));
        return ret;
    }

    // Wait for measurement (high precision: ~8.3ms)
    vTaskDelay(pdMS_TO_TICKS(10));

    // Read data
    ret = i2c_master_receive(sht40_dev_handle, data, 6, pdMS_TO_TICKS(10));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read SHT40 data: %s", esp_err_to_name(ret));
        return ret;
    }

    // Verify temperature CRC
    uint8_t temp_crc = calculate_crc(data, 2);
    if (temp_crc != data[2]) {
        ESP_LOGE(TAG, "Temperature CRC mismatch: expected 0x%02X, got 0x%02X", temp_crc, data[2]);
        return ESP_ERR_INVALID_CRC;
    }

    // Verify humidity CRC
    uint8_t hum_crc = calculate_crc(&data[3], 2);
    if (hum_crc != data[5]) {
        ESP_LOGE(TAG, "Humidity CRC mismatch: expected 0x%02X, got 0x%02X", hum_crc, data[5]);
        return ESP_ERR_INVALID_CRC;
    }

    // Convert raw values to physical units
    uint16_t temp_raw = (data[0] << 8) | data[1];
    uint16_t hum_raw = (data[3] << 8) | data[4];

    // Temperature conversion: T = -45 + 175 * (raw / 65535)
    *temperature = -45.0f + 175.0f * ((float) temp_raw / 65535.0f);

    // Humidity conversion: RH = -6 + 125 * (raw / 65535)
    *humidity = -6.0f + 125.0f * ((float) hum_raw / 65535.0f);

    ESP_LOGD(TAG, "Temperature: %.2f°C, Humidity: %.2f%%", *temperature, *humidity);

    return ESP_OK;
}

bool sht40_is_available(void)
{
    return sensor_available;
}
