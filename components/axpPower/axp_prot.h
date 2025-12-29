#ifndef AXP_PROT_H
#define AXP_PROT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void axp_i2c_prot_init(void);
void axp_cmd_init(void);
void axp_basic_sleep_start(void);
//void state_axp2101_task(void *arg);
void axp2101_isCharging_task(void *arg);

// Battery status functions
int axp_get_battery_percent(void);
int axp_get_battery_voltage(void);
bool axp_is_charging(void);
bool axp_is_battery_connected(void);
bool axp_is_usb_connected(void);

// Power control functions
void axp_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif 