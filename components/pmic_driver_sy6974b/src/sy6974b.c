#include "sy6974b.h"

#include "driver/i2c_master.h"
#include "esp_log.h"

static const char *TAG = "pmic_sy6974b";

// SY6974B I2C address
#define SY6974B_I2C_ADDR 0x6B

// SY6974B register definitions
#define SY6974B_REG_STATUS 0x08  // System status register
#define SY6974B_VBUS_STAT_MASK 0xC0
#define SY6974B_CHRG_STAT_MASK 0x18

static i2c_master_dev_handle_t sy6974b_dev = NULL;

static esp_err_t sy6974b_read_reg(uint8_t reg, uint8_t *val)
{
    if (!sy6974b_dev)
        return ESP_ERR_INVALID_STATE;
    return i2c_master_transmit_receive(sy6974b_dev, &reg, 1, val, 1, pdMS_TO_TICKS(100));
}

void sy6974b_init(i2c_master_bus_handle_t i2c_bus)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SY6974B_I2C_ADDR,
        .scl_speed_hz = 100000,
    };
    esp_err_t ret = i2c_master_bus_add_device(i2c_bus, &dev_cfg, &sy6974b_dev);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SY6974B not found: %s", esp_err_to_name(ret));
        sy6974b_dev = NULL;
    } else {
        ESP_LOGI(TAG, "SY6974B initialized");
    }
}

int sy6974b_get_battery_voltage(void)
{
    // Voltage measurement is handled by board-level ADC; not available here
    return -1;
}

int sy6974b_get_battery_percent(void)
{
    // Percentage estimation is handled by board-level ADC; not available here
    return -1;
}

bool sy6974b_is_battery_connected(void)
{
    // Battery connection check is handled by board-level ADC; not available here
    return false;
}

bool sy6974b_is_charging(void)
{
    if (!sy6974b_dev)
        return false;

    uint8_t status = 0;
    if (sy6974b_read_reg(SY6974B_REG_STATUS, &status) != ESP_OK)
        return false;

    // CHRG_STAT bits [4:3]: 00=not charging, 01=pre-charge, 10=fast charging, 11=done
    uint8_t chrg_stat = (status & SY6974B_CHRG_STAT_MASK) >> 3;
    return (chrg_stat == 0x01 || chrg_stat == 0x02);
}

bool sy6974b_is_usb_connected(void)
{
    if (sy6974b_dev) {
        uint8_t status = 0;
        if (sy6974b_read_reg(SY6974B_REG_STATUS, &status) == ESP_OK) {
            // VBUS_STAT bits [7:6]: 00=no input, 01=USB SDP, 10=USB CDP, 11=USB DCP
            uint8_t vbus_stat = (status & SY6974B_VBUS_STAT_MASK) >> 6;
            return (vbus_stat != 0);
        }
    }
    // No I2C device or read failed — assume not connected
    return false;
}

void sy6974b_shutdown(void)
{
    // Shutdown is handled at board level (de-assert power enable pin)
    ESP_LOGI(TAG, "SY6974B shutdown requested");
}
