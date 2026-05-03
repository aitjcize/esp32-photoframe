#ifndef SPLASH_SCREEN_H
#define SPLASH_SCREEN_H

#include "esp_err.h"

/**
 * Display the OOBE splash screen with WiFi QR code.
 *
 * Loads the pre-rendered splash EPDGZ (gradient background, icon, app QR),
 * generates a WiFi QR code for the device's unique hotspot SSID,
 * composites the QR code onto the splash buffer, and displays the result.
 */
esp_err_t splash_screen_display(void);

/**
 * Display the setup-complete screen after first successful WiFi connection.
 *
 * Shows the device hostname as a QR code (http://hostname.local) and
 * a prompt to use the companion app or web interface.
 */
esp_err_t splash_screen_display_setup_complete(const char *hostname);

/**
 * Display the AP-mode splash with a dual QR layout: a WiFi-join QR for the
 * device's persistent hotspot on the left, and a URL QR for the main
 * webapp (http://192.168.4.1) on the right.
 */
esp_err_t splash_screen_display_ap_mode(const char *ssid, const char *password);

#endif
