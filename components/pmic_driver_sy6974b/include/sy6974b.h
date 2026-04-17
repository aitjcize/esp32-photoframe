#ifndef SY6974B_H
#define SY6974B_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "driver/i2c_master.h"

void sy6974b_init(i2c_master_bus_handle_t i2c_bus);
int sy6974b_get_battery_percent(void);
int sy6974b_get_battery_voltage(void);
bool sy6974b_is_charging(void);
bool sy6974b_is_battery_connected(void);
bool sy6974b_is_usb_connected(void);
void sy6974b_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
