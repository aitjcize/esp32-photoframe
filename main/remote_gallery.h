#ifndef REMOTE_GALLERY_H
#define REMOTE_GALLERY_H

#include "esp_err.h"

/**
 * @brief Download the remote gallery image to SD card
 *
 * Downloads the pre-processed BMP image from REMOTE_GALLERY_URL
 * and saves it to TEMP_IMAGE_PATH on the SD card.
 *
 * @return esp_err_t ESP_OK on success, ESP_FAIL on network/write error
 */
esp_err_t remote_gallery_download_image(void);

#endif
