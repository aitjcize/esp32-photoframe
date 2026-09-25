#pragma once
#include "adc_cali.h"
#include "adc_oneshot.h"
#define ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED 1
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    adc_unit_t unit_id;
    adc_channel_t chan;
    adc_atten_t atten;
    int bitwidth;
} adc_cali_curve_fitting_config_t;
esp_err_t adc_cali_create_scheme_curve_fitting(const adc_cali_curve_fitting_config_t *,
                                               adc_cali_handle_t *);
esp_err_t adc_cali_delete_scheme_curve_fitting(adc_cali_handle_t);
#ifdef __cplusplus
}
#endif
