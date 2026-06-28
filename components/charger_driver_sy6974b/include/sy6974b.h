#ifndef SY6974B_CHARGER_H
#define SY6974B_CHARGER_H

#include <stdbool.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the SY6974B charger on the given I2C bus.
 *
 * Adds the device at 0x6B and probes its status register. If the chip does not
 * ACK (e.g. a board revision that ships a different charger such as the
 * ETA6003), the driver records itself as unavailable and the query functions
 * below return safe defaults — callers don't need to special-case the board.
 *
 * @param i2c_bus I2C bus handle the SY6974B is wired to.
 * @return ESP_OK if the SY6974B responded, an error code otherwise.
 */
esp_err_t sy6974b_init(i2c_master_bus_handle_t i2c_bus);

/** @return true if a SY6974B was detected during init. */
bool sy6974b_is_available(void);

/** @return true while the battery is actively charging (pre-charge or fast). */
bool sy6974b_is_charging(void);

/** @return true when charging has completed (battery full, input still present). */
bool sy6974b_is_charge_done(void);

/** @return true when valid input power (VBUS) is present (power-good). */
bool sy6974b_is_power_good(void);

#ifdef __cplusplus
}
#endif

#endif  // SY6974B_CHARGER_H
