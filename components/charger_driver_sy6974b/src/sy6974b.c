#include "sy6974b.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "sy6974b";

// Silergy SY6974B single-cell switching charger. I2C address per datasheet.
#define SY6974B_I2C_ADDR 0x6B

// REG08 (STAT0): system status.
//   bits [4:3] CHRG_STAT, bit [2] PG_STAT.
#define SY6974B_REG08_STAT0 0x08
#define SY6974B_CHRG_STAT_SHIFT 3
#define SY6974B_CHRG_STAT_MASK 0x03
#define SY6974B_CHRG_STAT_NOT_CHARGING 0
#define SY6974B_CHRG_STAT_PRE_CHARGE 1
#define SY6974B_CHRG_STAT_FAST_CHARGE 2
#define SY6974B_CHRG_STAT_DONE 3
#define SY6974B_PG_STAT_BIT (1 << 2)

static bool initialized = false;
static bool available = false;
static i2c_master_dev_handle_t dev_handle = NULL;

// Read a single register. Returns ESP_OK on success.
static esp_err_t sy6974b_read_reg(uint8_t reg, uint8_t *value)
{
    if (!dev_handle)
        return ESP_ERR_INVALID_STATE;
    return i2c_master_transmit_receive(dev_handle, &reg, 1, value, 1, pdMS_TO_TICKS(100));
}

// Returns the 2-bit CHRG_STAT field, or -1 if the read failed.
static int sy6974b_charge_status(void)
{
    if (!available)
        return -1;
    uint8_t stat;
    if (sy6974b_read_reg(SY6974B_REG08_STAT0, &stat) != ESP_OK)
        return -1;
    return (stat >> SY6974B_CHRG_STAT_SHIFT) & SY6974B_CHRG_STAT_MASK;
}

esp_err_t sy6974b_init(i2c_master_bus_handle_t i2c_bus)
{
    ESP_LOGI(TAG, "Initializing SY6974B charger");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SY6974B_I2C_ADDR,
        .scl_speed_hz = 100000,
    };

    esp_err_t ret = i2c_master_bus_add_device(i2c_bus, &dev_cfg, &dev_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add SY6974B device: %s", esp_err_to_name(ret));
        dev_handle = NULL;
        available = false;
        initialized = true;
        return ret;
    }

    // Probe: read the status register. A board that ships a different charger
    // (e.g. ETA6003) won't ACK here, so we just mark ourselves unavailable.
    uint8_t stat;
    ret = sy6974b_read_reg(SY6974B_REG08_STAT0, &stat);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SY6974B not present (no ACK at 0x%02x): %s", SY6974B_I2C_ADDR,
                 esp_err_to_name(ret));
        // Release the device handle so the bus stays clean for other devices.
        i2c_master_bus_rm_device(dev_handle);
        dev_handle = NULL;
        available = false;
        initialized = true;
        return ret;
    }

    available = true;
    initialized = true;
    ESP_LOGI(TAG, "SY6974B detected (STAT0=0x%02x)", stat);
    return ESP_OK;
}

bool sy6974b_is_available(void)
{
    return available;
}

bool sy6974b_is_charging(void)
{
    int s = sy6974b_charge_status();
    return s == SY6974B_CHRG_STAT_PRE_CHARGE || s == SY6974B_CHRG_STAT_FAST_CHARGE;
}

bool sy6974b_is_charge_done(void)
{
    return sy6974b_charge_status() == SY6974B_CHRG_STAT_DONE;
}

bool sy6974b_is_power_good(void)
{
    if (!available)
        return false;
    uint8_t stat;
    if (sy6974b_read_reg(SY6974B_REG08_STAT0, &stat) != ESP_OK)
        return false;
    return (stat & SY6974B_PG_STAT_BIT) != 0;
}
