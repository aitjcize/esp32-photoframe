#include "sd_power.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Long enough for the rail to fall below the card's reset threshold even
// with no active discharge on the switch.
#define SD_POWER_OFF_MS 200

void sd_power_discharge(gpio_num_t pwr_pin, gpio_num_t cs_pin)
{
    // Levels first so enabling the outputs can't glitch the rail or CS high.
    gpio_set_level(pwr_pin, 0);
    gpio_set_level(cs_pin, 0);
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << pwr_pin) | (1ULL << cs_pin),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&cfg);
    vTaskDelay(pdMS_TO_TICKS(SD_POWER_OFF_MS));
    gpio_set_level(cs_pin, 1);
}
