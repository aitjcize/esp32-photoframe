#pragma once
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef void *adc_cali_handle_t;
esp_err_t adc_cali_raw_to_voltage(adc_cali_handle_t, int, int *);
#ifdef __cplusplus
}
#endif
