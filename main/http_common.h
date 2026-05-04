#ifndef HTTP_COMMON_H
#define HTTP_COMMON_H

#include "esp_err.h"
#include "esp_http_server.h"

// Handlers shared between the provisioning HTTP server (used during initial
// setup) and the main webapp HTTP server (used in normal operation, both
// STA and persistent AP modes). Each handler is self-contained and safe to
// register on either server.

// GET /api/wifi/scan — scan nearby WiFi networks, return a JSON array of
// {ssid, rssi, auth} entries deduplicated by SSID and sorted by RSSI.
// Briefly switches the radio to APSTA when called from AP mode so the
// scan runs without dropping AP clients.
esp_err_t http_common_wifi_scan_handler(httpd_req_t *req);

#endif
