#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

// Call at boot before dispatch. True means a deadline recovery must sleep
// without entering the cold-boot Wi-Fi/provisioning path.
bool network_wake_init(void);
esp_err_t network_wake_begin(void);
bool network_wake_active(void);
int network_wake_timeout_ms(int maximum_ms);
int64_t network_wake_deadline_us(int maximum_ms);
void network_wake_succeeded(void);
uint32_t network_wake_backoff_seconds(void);
