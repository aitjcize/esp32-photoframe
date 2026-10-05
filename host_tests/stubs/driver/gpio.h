// Host-test stub for driver/gpio.h — config.h includes it but the tested
// code touches no GPIO symbols.
#pragma once

typedef int gpio_num_t;

#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t gpio_set_level(gpio_num_t pin, unsigned int level);
#ifdef __cplusplus
}
#endif
