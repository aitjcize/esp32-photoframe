#pragma once
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef int adc_unit_t;
typedef int adc_channel_t;
typedef int adc_atten_t;
typedef void *adc_oneshot_unit_handle_t;
#define ADC_UNIT_1 0
#define ADC_CHANNEL_0 0
#define ADC_ATTEN_DB_12 12
#define ADC_BITWIDTH_DEFAULT 0
#define ADC_DIGI_CLK_SRC_DEFAULT 0
typedef struct {
    adc_unit_t unit_id;
    int clk_src;
} adc_oneshot_unit_init_cfg_t;
typedef struct {
    int bitwidth;
    adc_atten_t atten;
} adc_oneshot_chan_cfg_t;
esp_err_t adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t *, adc_oneshot_unit_handle_t *);
esp_err_t adc_oneshot_config_channel(adc_oneshot_unit_handle_t, adc_channel_t,
                                     const adc_oneshot_chan_cfg_t *);
esp_err_t adc_oneshot_read(adc_oneshot_unit_handle_t, adc_channel_t, int *);
esp_err_t adc_oneshot_del_unit(adc_oneshot_unit_handle_t);
#ifdef __cplusplus
}
#endif
