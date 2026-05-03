#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include "esp_err.h"

esp_err_t http_server_init(void);
esp_err_t http_server_stop(void);
void http_server_set_ready(void);

// Register captive-portal URL handlers + start the DNS catch-all so that
// phones joining the device's AP automatically launch the webapp. Only
// meaningful in persistent AP-only mode; safe to call at most once after
// http_server_init.
esp_err_t http_server_enable_captive_portal(void);

#endif
