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
5. `last_update_id` is persisted in NVS (+1 offset) so restarts never re-process old messages, and
   an allowlisted chat ID filters out unsolicited senders.
6. Filenames are short and collision-safe: `img_<unix-timestamp>.<ext>`.

**Wake-up processing order** (fixed, so behavior is predictable across timer, button, and
Telegram-triggered wakes):

```
WiFi connect → poll Telegram → emergency-reset scan → download & display newest image
  → run queued "/" commands → persist last_update_id → deep sleep
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
nothing is lost.

## Commands

| Command | Effect |
|---|---|
| `/status` | Firmware, reset reason, battery, WiFi, storage %, heap %, rotation schedule, and all toggle states |
| `/clear` | Clears the display to white |
| `/restart` | Restarts the device |
| `/pairing` | Toggles Hoch-/Querformat (portrait/landscape) pairing |
| `/rotate_cron <M H Weekday>` | Sets the auto-rotate schedule as a cron expression |
| `/deep_sleep on\|off` | Enables/disables deep sleep |
| `/auto_rotate on\|off` | Enables/disables the auto-rotate timer |
| `/wake_notify on\|off` | Toggles a status ping sent on every wake-up |
| `/error_overlay on\|off` | Toggles an on-display warning banner after repeated WiFi failures |
| `/wifi_perf on\|off` | Toggles the WiFi performance mode (see below) |
| `/help` | Lists all commands |
| `/telegram_reset` | **Emergency**: clears the queue immediately, highest priority |

Images can also be sent with a caption starting with `/` — the caption is treated as a command
instead of being drawn on the image.

### `/status` and message formatting

All bot replies use a consistent, scannable layout:

- `/status` groups related fields (firmware/reset, battery/WiFi, storage/heap, schedule,
  settings) with blank lines instead of one dense block, and reports storage and heap as both
  absolute values and percentages (e.g. `62.3/128.0 MB frei (48%)`).
- `/status` lists the on/off state of every toggle (`Pairing`, `Deep Sleep`, `Auto-Rotate`,
  `Wach-Auf-Meldung`, `Fehler-Overlay`, `WLAN-Performance`) as `[x]` / `[ ]`.
- Every reply is prefixed with `[OK]`, `[FEHLER]`, `[!]`, or `[i]` so success, failure, warning,
  and usage-hint messages are visually distinct at a glance.
- `/help` is grouped into **Status / Anzeige / Einstellungen / Notfall** sections.

Text stays plain ASCII (no Markdown parse mode, no umlauts/emoji) by design — Telegram's
`parse_mode` would require escaping user-controlled text like SSIDs and cron expressions to avoid
silently failing to send.

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
| Pairing | on | Combine mismatched-orientation images instead of letterboxing |
| Deep Sleep | on | Existing setting, now also controllable via `/deep_sleep` |
| Auto-Rotate | on | Existing setting, now also controllable via `/auto_rotate` |
| Wake notify | off | Status ping to Telegram on every wake |
| Error overlay | off | On-display warning banner after persistent WiFi failures |
| WiFi performance mode | off | See below |
| Home Assistant integration | **off** | Master switch for all HA features (see below) |
| OTA auto-check | on | Automatic update check on cold boot |
| Thumbnail gallery (Web UI) | **off** | Client-side only; large galleries slow down the device's HTTP server |

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
