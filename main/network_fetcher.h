#ifndef NETWORK_FETCHER_H
#define NETWORK_FETCHER_H

#include "esp_err.h"

esp_err_t network_fetch_image(const char *url, const char *dest_path);

#endif  // NETWORK_FETCHER_H
