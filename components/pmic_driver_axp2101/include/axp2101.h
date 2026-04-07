#ifndef AXP_PROT_H
#define AXP_PROT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "driver/gpio.h"
#include "driver/i2c_master.h"

void axp2101_init(i2c_master_bus_handle_t i2c_bus, gpio_num_t irq_pin);
void axp2101_cmd_init(void);
void axp2101_basic_sleep_start(void);
// void state_axp2101_task(void *arg);
void axp2101_isCharging_task(void *arg);

// Battery status functions
int axp2101_get_battery_percent(void);
int axp2101_get_battery_voltage(void);
bool axp2101_is_charging(void);
bool axp2101_is_battery_connected(void);
bool axp2101_is_usb_connected(void);

// Power control functions
void axp2101_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif