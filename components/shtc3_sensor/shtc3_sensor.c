#include "shtc3_sensor.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "shtc3_sensor";

#define SHTC3_I2C_ADDR 0x70

// SHTC3 Commands
#define SHTC3_CMD_WAKEUP 0x3517
#define SHTC3_CMD_SLEEP 0xB098
#define SHTC3_CMD_SOFT_RESET 0x805D
#define SHTC3_CMD_READ_ID 0xEFC8
#define SHTC3_CMD_MEASURE_NORMAL 0x7866  // Normal mode, T first, clock stretching disabled

static i2c_master_dev_handle_t shtc3_dev_handle = NULL;
static bool sensor_initialized = false;
static bool sensor_available = false;

// CRC-8 calculation for SHTC3 (polynomial: 0x31, init: 0xFF)
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

esp_err_t shtc3_init(i2c_master_bus_handle_t i2c_bus)
{
    esp_err_t ret;
    uint8_t cmd[2];
    uint8_t id_data[3];

    ESP_LOGI(TAG, "Initializing SHTC3 sensor");

    // Create device handle
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SHTC3_I2C_ADDR,
        .scl_speed_hz = 100000,
    };

    ret = i2c_master_bus_add_device(i2c_bus, &dev_cfg, &shtc3_dev_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add SHTC3 device: %s", esp_err_to_name(ret));
        return ret;
    }

    // Wake up sensor
    cmd[0] = (SHTC3_CMD_WAKEUP >> 8) & 0xFF;
    cmd[1] = SHTC3_CMD_WAKEUP & 0xFF;
    ret = i2c_master_transmit(shtc3_dev_handle, cmd, 2, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to wake up SHTC3: %s", esp_err_to_name(ret));
        sensor_available = false;
        sensor_initialized = true;
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(1));  // Wait for wake-up

    // Read ID to verify sensor presence
    cmd[0] = (SHTC3_CMD_READ_ID >> 8) & 0xFF;
    cmd[1] = SHTC3_CMD_READ_ID & 0xFF;
    ret = i2c_master_transmit_receive(shtc3_dev_handle, cmd, 2, id_data, 3, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read SHTC3 ID: %s", esp_err_to_name(ret));
        sensor_available = false;
        sensor_initialized = true;
        return ret;
    }

    // Verify CRC
    uint8_t crc = calculate_crc(id_data, 2);
    if (crc != id_data[2]) {
        ESP_LOGE(TAG, "SHTC3 ID CRC mismatch: expected 0x%02X, got 0x%02X", crc, id_data[2]);
        sensor_available = false;
        sensor_initialized = true;
        return ESP_ERR_INVALID_CRC;
    }

    uint16_t id = (id_data[0] << 8) | id_data[1];
    ESP_LOGI(TAG, "SHTC3 sensor detected, ID: 0x%04X", id);

    sensor_available = true;
    sensor_initialized = true;
    return ESP_OK;
}

esp_err_t shtc3_read(float *temperature, float *humidity)
{
    if (!sensor_initialized) {
        ESP_LOGE(TAG, "SHTC3 not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (!sensor_available) {
        ESP_LOGD(TAG, "SHTC3 sensor not available");
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t ret;
    uint8_t cmd[2];
    uint8_t data[6];  // T_MSB, T_LSB, T_CRC, RH_MSB, RH_LSB, RH_CRC

    // Wake up sensor
    cmd[0] = (SHTC3_CMD_WAKEUP >> 8) & 0xFF;
    cmd[1] = SHTC3_CMD_WAKEUP & 0xFF;
    ret = i2c_master_transmit(shtc3_dev_handle, cmd, 2, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to wake up SHTC3: %s", esp_err_to_name(ret));
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(1));  // Wait for wake-up

    // Trigger measurement
    cmd[0] = (SHTC3_CMD_MEASURE_NORMAL >> 8) & 0xFF;
    cmd[1] = SHTC3_CMD_MEASURE_NORMAL & 0xFF;
    ret = i2c_master_transmit(shtc3_dev_handle, cmd, 2, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to trigger SHTC3 measurement: %s", esp_err_to_name(ret));
        return ret;
    }

    // Wait for measurement to complete (typical: 12.1ms for normal mode)
    vTaskDelay(pdMS_TO_TICKS(15));

    // Read measurement data
    ret = i2c_master_receive(shtc3_dev_handle, data, 6, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read SHTC3 data: %s", esp_err_to_name(ret));
        return ret;
    }

    // Put sensor back to sleep to save power
    cmd[0] = (SHTC3_CMD_SLEEP >> 8) & 0xFF;
    cmd[1] = SHTC3_CMD_SLEEP & 0xFF;
    i2c_master_transmit(shtc3_dev_handle, cmd, 2, pdMS_TO_TICKS(100));

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

    // Humidity conversion: RH = 100 * (raw / 65535)
    *humidity = 100.0f * ((float) hum_raw / 65535.0f);

    ESP_LOGD(TAG, "Temperature: %.2f°C, Humidity: %.2f%%", *temperature, *humidity);

    return ESP_OK;
}

bool shtc3_is_available(void)
{
    return sensor_available;
}

esp_err_t shtc3_sleep(void)
{
    if (!sensor_initialized) {
        ESP_LOGD(TAG, "SHTC3 not initialized, skip sleep");
        return ESP_ERR_INVALID_STATE;
    }

    if (!sensor_available) {
        ESP_LOGD(TAG, "SHTC3 not available, skip sleep");
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t cmd[2];
    cmd[0] = (SHTC3_CMD_SLEEP >> 8) & 0xFF;
    cmd[1] = SHTC3_CMD_SLEEP & 0xFF;
    esp_err_t ret = i2c_master_transmit(shtc3_dev_handle, cmd, 2, pdMS_TO_TICKS(100));

    if (ret == ESP_OK) {
        ESP_LOGD(TAG, "SHTC3 put to sleep");
    } else {
        ESP_LOGW(TAG, "Failed to put SHTC3 to sleep: %s", esp_err_to_name(ret));
    }

    return ret;
}

esp_err_t shtc3_wakeup(void)
{
    if (!sensor_initialized) {
        ESP_LOGD(TAG, "SHTC3 not initialized, skip wakeup");
        return ESP_ERR_INVALID_STATE;
    }

    if (!sensor_available) {
        ESP_LOGD(TAG, "SHTC3 not available, skip wakeup");
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t cmd[2];
    cmd[0] = (SHTC3_CMD_WAKEUP >> 8) & 0xFF;
    cmd[1] = SHTC3_CMD_WAKEUP & 0xFF;
    esp_err_t ret = i2c_master_transmit(shtc3_dev_handle, cmd, 2, pdMS_TO_TICKS(100));

    if (ret == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(1));  // Wait for wake-up
        ESP_LOGD(TAG, "SHTC3 woken up");
    } else {
        ESP_LOGW(TAG, "Failed to wake up SHTC3: %s", esp_err_to_name(ret));
    }

    return ret;
}
