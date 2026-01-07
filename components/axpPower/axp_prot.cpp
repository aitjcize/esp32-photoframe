#include "axp_prot.h"

#include <stdio.h>

#include "XPowersLib.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "i2c_bsp.h"

const char *TAG = "axp2101";

static XPowersPMU axp2101;

static int AXP2101_SLAVE_Read(uint8_t devAddr, uint8_t regAddr, uint8_t *data, uint8_t len)
{
    int ret;
    uint8_t count = 3;
    do {
        ret = (i2c_read_buff(axp2101_dev_handle, regAddr, data, len) == ESP_OK) ? 0 : -1;
        if (ret == 0)
            break;
        vTaskDelay(pdMS_TO_TICKS(100));
        count--;
    } while (count);
    return ret;
}

static int AXP2101_SLAVE_Write(uint8_t devAddr, uint8_t regAddr, uint8_t *data, uint8_t len)
{
    int ret;
    uint8_t count = 3;
    do {
        ret = (i2c_write_buff(axp2101_dev_handle, regAddr, data, len) == ESP_OK) ? 0 : -1;
        if (ret == 0)
            break;
        vTaskDelay(pdMS_TO_TICKS(100));
        count--;
    } while (count);
    return ret;
}

void axp_i2c_prot_init(void)
{
    if (axp2101.begin(AXP2101_SLAVE_ADDRESS, AXP2101_SLAVE_Read, AXP2101_SLAVE_Write)) {
        ESP_LOGI(TAG, "Init PMU SUCCESS!");
    } else {
        ESP_LOGE(TAG, "Init PMU FAILED!");
    }
}

void axp_cmd_init(void)
{
    ///* Set up the startup battery temperature management. disableTSPinMeasure() - Disable battery
    /// temperature measurement. */
    axp2101.disableTSPinMeasure();
    int data = axp2101.readRegister(0x26);
    ESP_LOGW("axp2101_init_log", "reg_26:0x%02x", data);
    if (data & 0x01) {
        axp2101.enableWakeup();
        ESP_LOGW("axp2101_init_log", "i2c_wakeup");
    }
    if (data & 0x08) {
        axp2101.wakeupControl(XPOWERS_AXP2101_WAKEUP_PWROK_TO_LOW, false);
        ESP_LOGW("axp2101_init_log",
                 "When setting the wake-up operation, pwrok does not need to be pulled down.");
    }
    if (axp2101.getPowerKeyPressOffTime() != XPOWERS_POWEROFF_4S) {
        axp2101.setPowerKeyPressOffTime(XPOWERS_POWEROFF_4S);
        ESP_LOGW("axp2101_init_log",
                 "Press and hold the pwr button for 4 seconds to shut down the device.");
    }
    if (axp2101.getPowerKeyPressOnTime() != XPOWERS_POWERON_128MS) {
        axp2101.setPowerKeyPressOnTime(XPOWERS_POWERON_128MS);
        ESP_LOGW("axp2101_init_log", "Click PWR to turn on the device.");
    }
    if (axp2101.getChargingLedMode() != XPOWERS_CHG_LED_OFF) {
        axp2101.setChargingLedMode(XPOWERS_CHG_LED_OFF);
        ESP_LOGW("axp2101_init_log", "Disable the CHGLED function.");
    }
    if (axp2101.getChargeTargetVoltage() != XPOWERS_AXP2101_CHG_VOL_4V1) {
        axp2101.setChargeTargetVoltage(XPOWERS_AXP2101_CHG_VOL_4V1);
        ESP_LOGW("axp2101_init_log", "Set the full charge voltage of the battery to 4.1V.");
    }
    if (axp2101.getButtonBatteryVoltage() != 3300) {
        axp2101.setButtonBatteryChargeVoltage(3300);
        ESP_LOGW("axp2101_init_log", "Set Button Battery charge voltage");
    }
    if (axp2101.isEnableButtonBatteryCharge() == 0) {
        axp2101.enableButtonBatteryCharge();
        ESP_LOGW("axp2101_init_log", "Enable Button Battery charge");
    }
    if (axp2101.getDC1Voltage() != 3300) {
        axp2101.setDC1Voltage(3300);
        ESP_LOGW("axp2101_init_log", "Set DCDC1 to output 3V3");
    }
    if (axp2101.getALDO3Voltage() != 3300) {
        axp2101.setALDO3Voltage(3300);
        ESP_LOGW("axp2101_init_log", "Set ALDO3 to output 3V3");
    }
    if (axp2101.getALDO4Voltage() != 3300) {
        axp2101.setALDO4Voltage(3300);
        ESP_LOGW("axp2101_init_log", "Set ALDO4 to output 3V3");
    }
}

int axp_get_battery_percent(void)
{
    if (axp2101.isBatteryConnect()) {
        return axp2101.getBatteryPercent();
    }
    return -1;
}

int axp_get_battery_voltage(void)
{
    return axp2101.getBattVoltage();
}

bool axp_is_charging(void)
{
    return axp2101.isCharging();
}

bool axp_is_battery_connected(void)
{
    return axp2101.isBatteryConnect();
}

bool axp_is_usb_connected(void)
{
    return axp2101.isVbusIn();
}

void axp_shutdown(void)
{
    ESP_LOGI(TAG, "Triggering hard power-off via AXP2101");
    axp2101.shutdown();
}

void axp_disable_charging(void)
{
    ESP_LOGI(TAG, "Disabling battery charging");
    axp2101.disableCellbatteryCharge();
}

void axp_enable_charging(void)
{
    ESP_LOGI(TAG, "Enabling battery charging");
    axp2101.enableCellbatteryCharge();
}

int axp_get_vbus_voltage(void)
{
    return axp2101.getVbusVoltage();
}

int axp_get_system_voltage(void)
{
    return axp2101.getSystemVoltage();
}

const char *axp_get_charger_status_string(void)
{
    uint8_t charge_status = axp2101.getChargerStatus();
    if (charge_status == XPOWERS_AXP2101_CHG_TRI_STATE) {
        return "tri_charge";
    } else if (charge_status == XPOWERS_AXP2101_CHG_PRE_STATE) {
        return "pre_charge";
    } else if (charge_status == XPOWERS_AXP2101_CHG_CC_STATE) {
        return "constant_charge";
    } else if (charge_status == XPOWERS_AXP2101_CHG_CV_STATE) {
        return "constant_voltage";
    } else if (charge_status == XPOWERS_AXP2101_CHG_DONE_STATE) {
        return "charge_done";
    } else if (charge_status == XPOWERS_AXP2101_CHG_STOP_STATE) {
        return "not_charging";
    }
    return "unknown";
}
