#pragma once

#include <stddef.h>

#include "esp_err.h"

// Fetch the TLS cert from `url` (https://host[:port][/path]), store it via
// config_manager_set_ca_cert_der, and call config_manager_touch_config.
//
// On success returns ESP_OK.
// On failure returns ESP_FAIL and, if `err_out` is non-NULL, copies a
// human-readable error into it (NUL-terminated), truncated to `err_out_len`.
// The stored cert is NOT modified on failure.
esp_err_t cert_pin_fetch_and_store(const char *url, char *err_out, size_t err_out_len);

// Clear any pinned cert.
void cert_pin_clear(void);
