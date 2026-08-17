#ifndef TELEGRAM_BOT_H
#define TELEGRAM_BOT_H

#include "esp_err.h"

// Result of a single telegram_bot_poll() call.
typedef enum {
    // Polled successfully. Any new images were downloaded (progressive
    // fallback applied) and the newest one displayed; any "/" commands found
    // were queued for telegram_bot_run_pending_commands().
    TELEGRAM_POLL_OK = 0,

    // The emergency "/telegram_reset" command was found in this batch. All
    // other updates in the batch were skipped, update_id was advanced past
    // the whole batch, and a confirmation was already sent. The caller MUST
    // skip straight to deep sleep - do not display images or run commands.
    TELEGRAM_POLL_RESET,

    // Bot token or chat ID not configured - nothing was done.
    TELEGRAM_POLL_NOT_CONFIGURED,

    // Network/HTTP/JSON error talking to the Telegram API.
    TELEGRAM_POLL_ERROR,
} telegram_poll_result_t;

// Poll Telegram (getUpdates) for updates newer than the persisted
// last_update_id, strictly filtered to the configured chat ID.
//
// Behavior (see telegram_poll_result_t for the RESET short-circuit):
//  - Downloads every new photo/image-document in the batch into
//    TELEGRAM_DOWNLOAD_DIRECTORY as "img_<unix-timestamp>.<ext>". For
//    Telegram "photo" messages (which carry several re-encoded resolutions,
//    largest last) the largest is tried first; on failure (too large,
//    network error, or an unsupported *progressive* JPEG - Telegram
//    re-encodes compressed photos as progressive JPEG, which this
//    firmware's JPEG decoder cannot decode) the next smaller size is tried,
//    down to the smallest, until one succeeds or all fail.
//  - The newest successfully downloaded image is run through the existing
//    processing pipeline and shown on the display; older images stay on
//    storage for later rotation cycles.
//  - Any message text starting with "/" (other than the reset command) is
//    queued as a pending command.
//  - Persists the highest update_id seen (+1 offset) to NVS so updates are
//    never re-processed.
esp_err_t telegram_bot_poll(telegram_poll_result_t *out_result);

// Runs every command queued by the last telegram_bot_poll() call, sending a
// sendMessage reply for each (e.g. /status, /restart, /clear). Call after
// image display, before deep sleep. No-op if the queue is empty. Note:
// /restart calls esp_restart() immediately and does not return.
void telegram_bot_run_pending_commands(void);

// Sends a free-form text message to the configured chat. Returns ESP_OK on
// success. No-op (ESP_ERR_INVALID_STATE) if Telegram isn't configured.
esp_err_t telegram_bot_send_message(const char *text);

#endif
