# Telegram Bot Integration

A rotation mode that receives images directly via the [Telegram Bot API](https://core.telegram.org/bots/api),
independent of the existing SD-card and URL-fetch rotation modes. Unlike the companion
[esp32-photoframe-server](https://github.com/aitjcize/esp32-photoframe-server)'s Telegram source
(which relays through a separate server), this integration talks to Telegram directly from the
firmware — no additional server required.

## How it works

1. On every deep-sleep wake (or auto-rotate timer), the device long-polls Telegram's `getUpdates`
   endpoint for new messages.
2. Images sent as a **photo** or as a **file/document** are accepted, in any format the firmware
   already supports.
3. Telegram re-encodes compressed photos as **progressive JPEG**, which the firmware's decoder
   cannot read. The bot automatically falls back through Telegram's other resolutions of the same
   photo (largest → smallest) until one succeeds, or asks for the image as a file instead.
4. A caption sent with the image is drawn onto the photo as a text overlay (unless the caption
   itself is a `/`-command).
5. Every downloaded image is converted to a processed, display-ready PNG (deleting the raw
   original) and gets a preview thumbnail, exactly like a manual Web UI upload — so it shows up
   correctly in the gallery and is a normal, rotatable album image, not just a one-off push.
6. `last_update_id` is persisted in NVS (+1 offset) so restarts never re-process old messages, and
   an allowlisted chat ID filters out unsolicited senders.
7. Filenames are short and collision-safe: `img_<unix-timestamp>.<ext>`.
8. If a poll doesn't result in a new image being displayed (nothing new arrived, or everything in
   the batch was a command / still waiting for its pairing partner), the device falls back to
   normal album rotation instead of leaving the previous image up indefinitely — the frame always
   changes image on every rotation-triggering wake, same as the non-Telegram modes.

**Wake-up processing order** (fixed, so behavior is predictable across timer, button, and
Telegram-triggered wakes):

```
WiFi connect → poll Telegram → emergency-reset scan → download & display newest image
  (or fall back to album rotation) → run queued "/" commands → persist last_update_id → deep sleep
```

## Emergency reset

`/telegram_reset` is checked **before** anything else in the queue. It immediately clears the
whole pending-image and pending-command queue and puts the device back to sleep, bypassing normal
processing — a safety valve if the queue gets flooded or stuck.

## Multi-image orientation pairing

If the display is in portrait orientation (from `display_rotation_deg`) but the frame's default
layout is landscape (or vice versa), a single portrait image would normally be letterboxed. When
**Pairing** (`/pairing`) is enabled, two images of complementary orientation are combined
side-by-side into one composed image instead. Unpaired images wait in a small persistent queue
(NVS-backed) until a matching partner arrives; the composed result is saved to the album so
nothing is lost. Within a single batch of several images, only the newest ready-to-display result
(a plain image or a freshly-composed pair) ends up on screen — everything else is still saved to
the album and available for later rotation.

The same pairing idea is also available for **normal auto-rotation** (not just Telegram receives)
— see [Auto-rotate orientation pairing](#auto-rotate-orientation-pairing) below.

## Display history (no-repeat rotation)

Random album rotation (Telegram's own album included, since downloaded images become normal album
files) tracks every image it has shown by path in a persisted history file, so it cycles through
every image in the active album(s) once before repeating, instead of just avoiding the single
immediately-previous pick. Once everything has been shown, the history clears itself and a new
cycle starts automatically. `/clear_history` clears it manually (and resets the sequential-mode
rotation cursor too).

## Commands

| Command | Effect |
|---|---|
| `/status` | Firmware, reset reason, battery, WiFi, storage %, heap %, rotation schedule, and all toggle states |
| `/clear` | Clears the display to white |
| `/restart` | Restarts the device |
| `/pairing` | Toggles portrait/landscape combining for Telegram receives |
| `/list_albums` | Lists every album, with its active/inactive state |
| `/active_albums` | Lists only the active albums |
| `/enable_album <name>` | Activates an existing album for rotation |
| `/clear_history` | Clears the display history and restarts the no-repeat cycle |
| `/rotate_cron <M H Weekday>` | Sets the auto-rotate schedule as a cron expression |
| `/deep_sleep on\|off` | Enables/disables deep sleep |
| `/auto_rotate on\|off` | Enables/disables the auto-rotate timer |
| `/wake_notify on\|off` | Toggles a status ping sent on every wake-up |
| `/error_overlay on\|off` | Toggles an on-display warning banner after repeated WiFi failures |
| `/wifi_perf on\|off` | Toggles the WiFi performance mode (see below) |
| `/rotation_pairing on\|off` | Toggles auto-rotate orientation pairing (random mode only) |
| `/help` | Lists all commands |
| `/telegram_reset` | **Emergency**: clears the queue immediately, highest priority |

Images can also be sent with a caption starting with `/` — the caption is treated as a command
instead of being drawn on the image.

### `/status` and message formatting

All bot replies use a consistent, scannable, plain-ASCII layout:

- `/status` groups related fields (firmware/reset, battery/WiFi, storage/heap, schedule,
  settings) with blank lines instead of one dense block, and reports storage and heap as both
  absolute values and percentages (e.g. `62.3/128.0 MB free (48%)`).
- `/status` lists the on/off state of every toggle as `[x]` / `[ ]`.
- Every reply is prefixed with `[OK]`, `[ERROR]`, `[!]`, or `[i]` so success, failure, warning,
  and usage-hint messages are visually distinct at a glance.
- `/help` is grouped into Status / Display / Albums / Settings / Emergency sections.

Text stays plain ASCII (no Markdown parse mode, no accented characters/emoji) by design —
Telegram's `parse_mode` would require escaping user-controlled text like SSIDs and cron
expressions to avoid silently failing to send.

## Low battery & wake notifications

- If the battery drops below 20%, a one-time warning is sent via Telegram even if there were no
  new messages to process (debounced — fires once per discharge cycle, clears again above 25%).
- Optional wake-up ping (`/wake_notify`): sends a full `/status`-style report to Telegram on every
  wake, so you can confirm the device is alive without opening the web UI.

## Settings (Web UI + Telegram)

All default to preserving existing behavior for users who don't configure Telegram at all.

| Setting | Default | Purpose |
|---|---|---|
| Telegram bot token / chat ID | empty | Enables the Telegram rotation mode when both are set |
| Pairing | on | Combine mismatched-orientation Telegram receives instead of letterboxing |
| Deep Sleep | on | Existing setting, now also controllable via `/deep_sleep` |
| Auto-Rotate | on | Existing setting, now also controllable via `/auto_rotate` |
| Wake notify | off | Status ping to Telegram on every wake |
| Error overlay | off | On-display warning banner after persistent WiFi failures |
| WiFi performance mode | off | See below |
| Home Assistant integration | **off** | Master switch for all HA features (see below) |
| OTA auto-check | on | Automatic update check on cold boot |
| Auto-rotate orientation pairing | **off** | See [below](#auto-rotate-orientation-pairing) - random mode only |
| Thumbnail gallery (Web UI) | **off** | Client-side toggle; large galleries slow down the device's HTTP server |

### WiFi performance mode

WiFi power-save is normally tiered: the radio only stays in full-receive mode while someone is
actively looking at the web UI, and drops to a power-saving mode otherwise. Enabling this toggle
forces full performance at all times, trading battery life for a consistently faster web UI —
useful for always-on / Home Assistant setups.

### Home Assistant master switch

All Home Assistant integration code checks a single `ha_enabled` flag before doing anything
(`ha_is_configured()` requires both the existing HA URL config **and** this flag). It defaults to
**off**, but for anyone who already had HA configured before this change, the migration path keeps
their integration working — the flag isn't silently sprung on existing users.

### OTA auto-check toggle

Disabling automatic OTA checks (`ota_check_enabled = false`) skips the cold-boot update check
entirely — useful for dev builds where a `dev-<commit>` version string otherwise causes spurious
"update available" comparisons.

### Auto-rotate orientation pairing

Extends the same portrait/landscape combining idea to **normal auto-rotation**, not just Telegram
receives. When enabled and the randomly-picked next image doesn't match the panel's orientation,
the device immediately looks for another mismatched image in the active album(s) and combines
them instead of showing one letterboxed. The combined result is saved permanently in the album
(with a thumbnail); both source images are kept too, still independently rotatable later.

**Random rotation mode only** — sequential mode's deterministic image-order cursor is deliberately
left untouched, so this setting has no effect there. The Web UI shows a warning if the toggle is
on while Sequential mode is selected.

### Error overlay test

Settings includes a "Test Error Overlay" button that triggers the overlay immediately
(`POST /api/error-overlay/test`), regardless of the setting above, to preview it without waiting
for 3 consecutive WiFi failures. If there's no current image to overlay onto (fresh boot, or after
`/clear`), it generates a blank canvas instead of failing, so the preview - and the real
WiFi-failure case on a device that's never displayed anything yet - always has something to show.

## Security

- **Redacted logging**: the bot token is never logged. All Telegram HTTP calls log through
  `redact_url_for_log()`, which strips the token before anything reaches the log.
- **Chat allowlist**: only messages from the configured chat ID are ever processed.
- Keep your bot token private — anyone with it can send commands to your device, including
  `/telegram_reset` and `/restart`.

## Setup

1. Talk to [@BotFather](https://t.me/BotFather) on Telegram, create a bot, and copy the token it
   gives you.
2. Message your new bot once (or add it to a group) so you have a chat ID; the simplest way to
   find it is to send a message and check `https://api.telegram.org/bot<TOKEN>/getUpdates`.
3. In the PhotoFrame web UI, go to **Settings → Telegram**, enter the bot token and chat ID, and
   enable the integration.
4. Send the bot a photo. It will be processed on the next wake (or trigger one immediately,
   depending on your rotation-timer settings).
