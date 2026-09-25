#ifndef BOARD_HAL_BATTERY_ADC_H
#define BOARD_HAL_BATTERY_ADC_H

#include <stdbool.h>

#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Configuration for measuring battery voltage through a resistor divider on an
// ESP32 ADC channel, optionally gated by a load-switch enable pin. Shared by
// every board that reads the battery this way (reTerminal E1002/E1004, XIAO
// EE02/EE04) so the calibration/averaging logic lives in one place.
typedef struct {
    adc_unit_t unit;        // ADC unit, e.g. ADC_UNIT_1
    adc_channel_t channel;  // ADC channel, e.g. ADC_CHANNEL_0
    adc_atten_t atten;      // attenuation, e.g. ADC_ATTEN_DB_12
    int enable_pin;         // load-switch enable GPIO (driven high to measure), or -1
    int settle_ms;          // delay after enabling the switch before sampling
    int samples;            // ADC samples averaged per read (>=1)
    float divider;          // voltage-divider ratio, e.g. 2.0
    float cal_scale;        // per-unit multimeter correction (1.0 = none)
    bool sample_at_create;  // capture a boot sample before the caller powers the display
} battery_adc_config_t;

typedef struct battery_adc battery_adc_t;

/**
 * @brief Create an ADC oneshot unit/channel with eFuse calibration.
 *
 * The enable pin (if any) must already be configured as an output by the caller;
 * this helper only toggles its level during a read. With sample_at_create,
 * creation also takes one reading (including a possible -1 result).
 *
 * @param cfg Configuration (copied internally).
 * @param out Receives the created handle on success.
 * @return ESP_OK on success, error code otherwise.
 */
esp_err_t battery_adc_create(const battery_adc_config_t *cfg, battery_adc_t **out);

/**
 * @brief Read battery voltage in millivolts (divider and cal_scale applied).
 *
 * Returns the boot sample when enabled via board_hal_use_boot_battery_sample()
 * and sample_at_create was requested. Otherwise takes a fresh reading:
 * enables the load switch, waits settle_ms, averages samples, converts via the
 * eFuse calibration (or a linear estimate if calibration was unavailable), then
 * disables the load switch.
 *
 * @return Battery voltage in mV, or -1 on failure.
 */
int battery_adc_read_mv(battery_adc_t *ctx);

/** @brief Tear down the ADC unit + calibration scheme and free the handle. */
void battery_adc_destroy(battery_adc_t *ctx);

#ifdef __cplusplus
}
#endif

#endif  // BOARD_HAL_BATTERY_ADC_H
