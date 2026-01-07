#ifndef AXP_PROT_H
#define AXP_PROT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void axp_i2c_prot_init(void);
void axp_cmd_init(void);

// Battery status functions
int axp_get_battery_percent(void);
int axp_get_battery_voltage(void);
bool axp_is_charging(void);
bool axp_is_battery_connected(void);
bool axp_is_usb_connected(void);

// Power control functions
void axp_shutdown(void);

// Charging control functions
void axp_disable_charging(void);
void axp_enable_charging(void);

// Diagnostic functions
int axp_get_vbus_voltage(void);
int axp_get_system_voltage(void);
const char *axp_get_charger_status_string(void);

#ifdef __cplusplus
}
#endif

#endif