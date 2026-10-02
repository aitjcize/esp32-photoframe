#ifndef BOARD_HAL_SD_POWER_H
#define BOARD_HAL_SD_POWER_H

#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

// Cut a microSD slot's rail (active-high load switch) for long enough to
// reset the card, with CS held low so the card is not back-powered through
// it, and leave the rail off with CS deselected. A reset without a power cut
// leaves the card wherever its last command stopped, and from there it
// ignores the init sequence. Call before anything drives the SPI bus the
// card shares, and power the rail back up only right before mounting: a
// powered card that is not yet in SPI mode listens to the shared bus and can
// answer on MOSI while the panel is being driven.
void sd_power_discharge(gpio_num_t pwr_pin, gpio_num_t cs_pin);

#ifdef __cplusplus
}
#endif

#endif  // BOARD_HAL_SD_POWER_H
