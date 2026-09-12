# PhotoFrame API Documentation

Complete REST API reference for the ESP32 PhotoFrame firmware.

## Base URL

All endpoints are relative to: `http://<device-ip>/`

---

## System

### `GET /api/system-info`

Get device system information.

**Response:**
```json
{
  "device_name": "PhotoFrame",
  "device_id": "aabbccddeeff",
  "board_name": "Waveshare 7.3\" 7-Color",
  "version": "2.7.0",
  "project_name": "esp32-photoframe",
  "compile_time": "12:34:56",
  "compile_date": "Apr 17 2026",
  "idf_version": "v5.3",
  "width": 800,
  "height": 480,
  "has_sdcard": true,
  "sdcard_inserted": true,
  "has_flash_storage": false,
  "storage_total": 31914983424,
  "storage_used": 1048576
}
```

### `GET /api/battery`

Get battery status (boards with AXP2101 PMIC).

**Response:**
```json
{
  "battery_level": 85,
  "battery_voltage": 4100,
  "charging": false,
  "usb_connected": true,
  "battery_connected": true
}
```

### `GET /api/sensor`

Get environmental sensor data (boards with SHT40/SHTC3).

**Response:**
```json
{
  "temperature": 25.3,
  "humidity": 45.2
}
```

### `GET /api/time`

Get device time.

**Response:**
```json
{
  "time": "2026-04-12T01:00:00+08:00",
  "timestamp": 1776124800
}
```

### `POST /api/time/sync`

Trigger NTP time sync.

**Response:**
```json
{
  "status": "success"
}
```

### `POST /api/keep_alive`

Reset the auto-sleep timer.

**Response:**
```json
{
  "status": "success"
}
```

---

## Configuration

### `GET /api/config`

Get current device configuration.

**Response:**
```json
{
  "device_name": "PhotoFrame",
  "device_id": "aabbccddeeff",
  "timezone": "UTC-8",
  "ntp_server": "pool.ntp.org",
  "wifi_ssid": "MyNetwork",
  "display_orientation": "landscape",
  "display_rotation_deg": 180,
  "auto_rotate": true,
  "rotate_cron": ["0 */12 *"],
  "rotation_mode": "url",
  "sd_rotation_mode": "random",
  "image_url": "http://server:9607/image/immich",
  "ca_cert_set": false,
  "last_fetch_error": "",
  "access_token": "",
  "http_header_key": "",
  "http_header_value": "",
  "save_downloaded_images": true,
  "ha_url": "",
  "openai_api_key": "",
  "google_api_key": "",
  "deep_sleep_enabled": true,
  "chime_enabled": true,
  "chime_supported": true,
  "chime_preset": "mozart",
  "chime_url": "",
  "chime_source": "preset",
  "chime_pull_mode": "with_rotate",
  "chime_play_when": "after",
  "chime_file": "",
  "chime_cached": false
}
```

**Fields:**
- `device_name`: Device name (used for mDNS hostname)
- `timezone`: POSIX timezone string (e.g., `UTC-8` for PST)
- `ntp_server`: NTP server address
- `display_orientation`: `"landscape"` or `"portrait"`
- `display_rotation_deg`: Display rotation in degrees (0, 90, 180, 270)
- `auto_rotate`: Enable automatic image rotation
- `rotate_cron`: Array of 1 to 7 simplified 3-field cron expressions
  (`minute hour day-of-week`). The next rotation is the earliest time matching
  any rule. Supports `*`, `a`, `a-b`, `*/n`, `a-b/n` and comma lists; day-of-week
  `0`/`7` = Sunday (7 is also valid in ranges, e.g. `5-7` = Fri–Sun). (Day-of-month
  and month are intentionally omitted.) Invalid expressions and empty arrays are
  rejected with `400` — turn `auto_rotate` off to stop rotating. For backward
  compatibility, `POST`/`PATCH` also accept a legacy `rotate_interval` (seconds)
  and convert it to a single cron rule. (To avoid rotating at night, bound the
  active hours in the cron rules, e.g. `0 7-23/2 *`, or split overnight coverage
  into two rules — there is no separate quiet-hours setting.)
- `rotation_mode`: `"storage"` (local SD/flash) or `"url"` (fetch from URL)
- `sd_rotation_mode`: `"random"` or `"sequential"`
- `image_url`: URL to fetch images from (max 256 chars)
- `ca_cert_set`: Whether a custom CA certificate is pinned for HTTPS
- `last_fetch_error`: Last image fetch error message (empty if no error)
- `access_token`: Bearer token for image URL authentication
- `http_header_key`/`http_header_value`: Custom HTTP header for image fetches
- `save_downloaded_images`: Save fetched images to Downloads album
- `ha_url`: Home Assistant URL for integration
- `openai_api_key`/`google_api_key`: AI API keys for client-side generation
- `deep_sleep_enabled`: Enable deep sleep between rotations
- `chime_enabled`: Master mute. Play a local ES8311 speaker chime on a successful image display / URL rotate (default `true`). Timing is `chime_play_when`. Set `false` for battery or quiet hours. Persisted in NVS as `chime_en`.
- `chime_supported`: Read-only. `true` on `waveshare_photopainter_73` (onboard ES8311 + PA). Other boards report `false`.
- `chime_preset`: Built-in synthesized public-domain tune (2–8 seconds): `mozart` (default, Eine kleine Nachtmusik opening), `ode` (Ode to Joy / Ode aan de vreugde), `frere` (Frère Jacques / Vader Jacob), `twinkle`, `fanfare`, `triad` (C–E–G flourish), `dingdong` (two-tone doorbell), `softping`, `alert`, `doublebeep`. `ascending` remains accepted as an alias of `fanfare`. Used when `chime_source` is `preset`, and as fallback if a WAV fetch/play fails. Persisted as `chime_preset`. Unknown names are rejected by `POST`/`PATCH /api/config`. Playback of an unknown HAL name falls back to `triad`.
- `chime_url`: Optional HTTP(S) URL of a small PCM WAV (similar to `image_url`, max 256 chars). Example: `http://news.local:8080/chime.wav`. Persisted as `chime_url`. Changing the URL clears the on-device cache.
- `chime_source`: `preset` (default) plays `chime_preset`. `wav` plays the last pulled WAV when `chime_url` is set. `uploaded` plays the file named by `chime_file` from `chimes/` on storage.
- `chime_file`: Filename of the active uploaded WAV (e.g. `doorbell.wav`). Empty when none is selected. Persisted as `chime_file`.
- `chime_pull_mode`: How `chime_url` is fetched when `chime_source` is `wav`.
  - `once`: reuse the on-device cache. Preview fetches if the cache is empty; **Pull now** (`POST /api/chime/pull`) GETs the URL on demand. On a display rotate, fetch only if nothing is cached.
  - `with_rotate` (default): GET `chime_url` at the same moment playback starts (`chime_play_when`), replace the cache, then play that WAV. Falls back to `chime_preset` if the fetch fails (existing cache is tried first).
- `chime_play_when`: When to start speaker playback relative to the e-ink image update. `POST`/`PATCH /api/config` accepts `before` or `after`; any other value is rejected with `400`.
  - `after` (default): do not start playback until `epaper_display()` has returned — that call is blocking (Power On → Send Data → Refresh → Power Off), so the chime (preset or WAV, including a `with_rotate` pull) starts only after the panel has finished drawing. Persisted as `chime_play_when`.
  - `before`: play after the new image has been decoded into the frame buffer, immediately before the panel wait. With `with_rotate`, the URL is GET-pulled at that earlier moment, then the WAV plays, then the ~30 s refresh begins.
- `chime_cached`: Read-only. `true` if a cached WAV is present on storage.

WAV limits (rejected/skipped gracefully): PCM only (not MP3/float), mono or stereo, 8- or 16-bit, 8–22.05 kHz, max **2 MiB** (`WAV_PCM_MAX_FILE_BYTES`, SD-backed cache; typical Pi bulletin files are ~1.2 MiB / ~27 s), first **60 seconds** played (`WAV_PCM_MAX_SECONDS`). Do not ship copyrighted OS ringtones in firmware; serve your own WAV from a Pi if you want a custom sound.

### `POST /api/config`

Update configuration. Only include fields to change.

**Request:**
```json
{
  "auto_rotate": true,
  "rotate_cron": ["*/30 8-22 1-5", "0 10 0,6"],
  "rotation_mode": "url",
  "image_url": "https://example.com/image"
}
```

**Response:**
```json
{
  "status": "success"
}
```

**TLS certificate pinning:** If the request changes `image_url` to an HTTPS URL different from the current value, the device fetches and pins that server's TLS certificate before applying the config. If the fetch fails, the request returns `400 Bad Request` with a `message` describing the failure and **no config changes are persisted**. Changing `image_url` to an HTTP URL or clearing it clears any previously pinned certificate. `GET /api/config` reports the current state via `ca_cert_set`.

### `PATCH /api/config`

Same as `POST /api/config`. Both methods accept partial updates.

**SD backup:** each successful `POST`/`PATCH` `/api/config` (and other calls that `touch` config, such as chime upload/delete) also writes `/storage/config/settings.json` when an SD card is mounted. That file is the backup that survives a full firmware flash at `0x0` (NVS wipe). On boot, NVS is loaded first; if the SD file exists and NVS has none of the backed-up keys, the snapshot is imported. Included keys: `auto_rotate`, `rotate_cron`, `rotation_mode`, `image_url`, `deep_sleep_enabled`, and all `chime_*` fields. WiFi is not stored here (`wifi.txt` is unchanged).

### `POST /api/config/export-sd`

Write the current runtime settings snapshot to `/storage/config/settings.json`. Returns `404` if no SD card is mounted.

### `POST /api/config/import-sd`

Read `/storage/config/settings.json` and apply it to NVS/runtime immediately (even if NVS is not factory-fresh). Returns `404` if the file is missing.

---

## Image Display

### `POST /api/display`

Display a specific image from storage.

**Request:**
```json
{
  "filepath": "Vacation/photo.bmp"
}
```

### `POST /api/display-image`

Upload and display an image directly. Supports JPEG, PNG, BMP, and EPDGZ formats.

**Single file:**
```bash
curl -X POST -H "Content-Type: image/jpeg" \
  --data-binary @photo.jpg \
  http://photoframe.local/api/display-image
```

**Multipart with thumbnail:**
```bash
curl -X POST \
  -F "image=@photo.jpg" \
  -F "thumbnail=@thumb.jpg" \
  http://photoframe.local/api/display-image
```

**Processing:**
- JPEG/PNG: decoded, dithered to e-paper palette, displayed
- BMP: displayed directly (must be pre-processed)
- EPDGZ: decompressed and displayed directly (4bpp gzipped raw data)

### `POST /api/rotate`

Trigger image rotation (respects rotation mode).

### `POST /api/chime`

Play the currently selected speaker chime (Waveshare PhotoPainter 7.3" only): the built-in `chime_preset`, the last cached URL WAV when `chime_source` is `wav`, or the uploaded file when `chime_source` is `uploaded`. Intended for a Raspberry Pi / remote trigger and for the Settings → Chimes preview button. Respects `chime_enabled`. Resets the auto-sleep timer.

When `chime_source` is `wav`, the device GET-pulls `chime_url` first if the cache is empty or the caller asked to refresh (`?refresh=1` or `{"refresh":true}`), then plays the WAV. It falls back to `chime_preset` only if fetch/validate/play fails. Upload and list selection apply immediately; the web UI also persists URL fields before preview.

**Response (played):**
```json
{
  "status": "success",
  "message": "Chime played",
  "played": "wav",
  "cached": true
}
```

`played` is `wav`, `preset`, or `uploaded`. If a WAV fetch failed and the preset was used instead, `message` is `"Chime played (preset fallback)"` and `error` describes the fetch failure.

**Response (disabled via config):**
```json
{
  "status": "disabled",
  "message": "Chime is disabled (set chime_enabled in /api/config)"
}
```

**Response (board has no speaker):** `404` with `"status": "unsupported"`.

### `POST /api/chime/pull`

GET `chime_url`, validate a supported PCM WAV, and replace the on-device cache. Does not play. Used by Settings → Chimes **Pull now / Nu ophalen** so `chime_pull_mode=once` can refresh without waiting for the first play. Resets the auto-sleep timer. Requires a non-empty `chime_url`.

```bash
curl -X POST http://photopainter.local/api/chime/pull
```

**Response:**
```json
{
  "status": "success",
  "message": "Chime WAV cached",
  "cached": true,
  "bytes": 1234567
}
```

On failure (`400`): `status` is `error`, `cached` reports whether a previous cache remains, and `message` explains the problem (empty URL, too large, unsupported format, HTTP error).

### `POST /api/chime/upload`

Upload a custom PCM WAV (multipart field `file` / `chime` / `image`, or raw `audio/wav` body). Stored under `chimes/` on SD (or flash if no SD). Same format limits as `chime_url` (PCM, 8–22.05 kHz, 8/16-bit, max 2 MiB / 60 seconds played). On success, that file becomes the active custom sound (`chime_source=uploaded`, `chime_file=<name>`). Optional `?name=doorbell.wav` for raw-body uploads.

```bash
curl -X POST -F 'file=@doorbell.wav' http://photopainter.local/api/chime/upload
curl -X POST -H 'Content-Type: audio/wav' --data-binary @doorbell.wav \
  'http://photopainter.local/api/chime/upload?name=doorbell.wav'
```

**Response:**
```json
{
  "status": "success",
  "message": "Chime uploaded",
  "filename": "doorbell.wav"
}
```

### `GET /api/chimes`

List uploaded chimes.

```json
{
  "chimes": [{ "name": "doorbell.wav", "size": 12340 }],
  "active": "doorbell.wav"
}
```

### `DELETE /api/chimes?name=doorbell.wav`

Delete an uploaded chime. If it was the active file, `chime_file` is cleared and source falls back to `preset`. Also accepts `{"name":"doorbell.wav"}` in the body.

### `GET /api/current_image`

Get the currently displayed image thumbnail.

### `POST /api/calibration/display`

Display the color calibration pattern on the e-paper.

---

## Image Management

### `GET /api/images?album=<name>`

List images in an album.

**Response:**
```json
[
  {
    "filename": "photo1.png",
    "album": "Vacation",
    "thumbnail": "photo1.jpg"
  }
]
```

### `GET /api/image?filepath=<album/filename>`

Serve an image file (thumbnail JPEG or fallback).

### `POST /api/upload`

Upload an image to an album.

- Content-Type: `multipart/form-data`
- Fields: `album` (text), `image` (file), `thumbnail` (file, optional)

### `POST /api/delete`

Delete an image.

**Request:**
```json
{
  "filepath": "Vacation/photo.png"
}
```

---

## Albums

### `GET /api/albums`

List all albums with enabled status.

**Response:**
```json
[
  { "name": "Default", "enabled": true },
  { "name": "Vacation", "enabled": false }
]
```

### `POST /api/albums`

Create an album.

**Request:**
```json
{
  "name": "Vacation"
}
```

### `DELETE /api/albums?name=<name>`

Delete an album and all its images.

### `PUT /api/albums/enabled?name=<name>`

Enable/disable an album for auto-rotation.

**Request:**
```json
{
  "enabled": true
}
```

---

## Processing Settings

### `GET /api/settings/processing`

Get image processing parameters.

**Response:**
```json
{
  "exposure": 1.0,
  "saturation": 1.0,
  "toneMode": "contrast",
  "contrast": 1.0,
  "strength": 0.5,
  "shadowBoost": 0.0,
  "highlightCompress": 0.0,
  "midpoint": 0.5,
  "colorMethod": "rgb",
  "ditherAlgorithm": "floyd-steinberg",
  "compressDynamicRange": true
}
```

### `POST /api/settings/processing`

Update processing parameters.

### `DELETE /api/settings/processing`

Reset to defaults.

---

## Color Palette

### `GET /api/settings/palette`

Get color palette calibration (RGB values for each e-paper color).

**Response:**
```json
{
  "black": { "r": 2, "g": 2, "b": 2 },
  "white": { "r": 190, "g": 200, "b": 200 },
  "yellow": { "r": 205, "g": 202, "b": 0 },
  "red": { "r": 135, "g": 19, "b": 0 },
  "blue": { "r": 5, "g": 64, "b": 158 },
  "green": { "r": 39, "g": 102, "b": 60 }
}
```

### `POST /api/settings/palette`

Update palette calibration.

### `DELETE /api/settings/palette`

Reset palette to defaults.

---

## Power Management

### `POST /api/sleep`

Enter deep sleep immediately.

**Response:**
```json
{
  "status": "success",
  "message": "Entering sleep mode"
}
```

---

## OTA Updates

### `GET /api/ota/status`

Get OTA update status.

**Response:**
```json
{
  "status": "idle",
  "current_version": "2.7.0",
  "latest_version": "2.7.0",
  "update_available": false
}
```

### `POST /api/ota/check`

Check for firmware updates.

### `POST /api/ota/update`

Start firmware update. Device reboots after completion.

---

## Storage Management

### `POST /api/format-storage`

Format the currently mounted storage filesystem. Supported for both
internal flash (LittleFS) and SD cards (FAT32). Returns `400` if the
active storage is neither (e.g. MemFS fallback when no persistent
storage is available).

### `POST /api/factory-reset`

Factory reset all settings to defaults. Erases NVS and deletes the SD settings snapshot (`/storage/config/settings.json`) so the next boot does not re-import the previous auto-rotate / chime backup. Photos on the card are left in place.

---

## URL Rotation Fetch

When `rotation_mode` is `"url"`, the device issues an HTTP `GET` against `image_url` on every rotation. This section describes that request — the headers sent, what the server can send back, and the caching / config-sync protocol.

**Transport:**
- Method: `GET`
- Timeout: 120s per attempt
- Retries: up to 3 attempts with a 3s delay between retries (failed status, zero-length body, or transport errors all trigger a retry)
- Redirects: up to 5 followed automatically
- User-Agent: `Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36`
- TLS: if a pinned CA certificate is set (`ca_cert_set: true` in `/api/config`), it is used to validate the server; otherwise the default trust store applies

### Request Headers

Every image-fetch request carries these headers so the server can tailor the response:

| Header | Description |
|--------|-------------|
| `X-Display-Width` | Native panel width in pixels (e.g., `800`) |
| `X-Display-Height` | Native panel height in pixels (e.g., `480`) |
| `X-Display-Orientation` | `landscape` or `portrait` |
| `X-Firmware-Version` | Firmware version string |
| `X-Config-Last-Updated` | Unix timestamp of the last local config change (used for remote sync reconciliation) |
| `X-Processing-Settings` | JSON blob of the current processing parameters |
| `X-Color-Palette` | JSON blob of the current color palette |

Authentication and custom headers, when configured via `/api/config`:

| Header | Description |
|--------|-------------|
| `Authorization` | `Bearer <access_token>` when `access_token` is non-empty |
| *custom* | The `http_header_key`/`http_header_value` pair, if both are non-empty. Skipped when the key is `Authorization` and an access token is already set (access token wins) |

Conditional request (ETag caching):

| Header | Description |
|--------|-------------|
| `If-None-Match` | The ETag persisted from the previous successful `200` response, if any. Enables the server to short-circuit with `304 Not Modified` when the image has not changed |

### Response Headers

The server can include these headers on a `200` response:

| Header | Description |
|--------|-------------|
| `X-Thumbnail-URL` | URL the device fetches next to store a companion thumbnail (30s timeout, no retries, no custom headers) |
| `X-Config-Payload` | JSON blob of config to merge into device state — see [Config Payload Structure](#config-payload-structure) |
| `ETag` | Opaque validator cached by the device and echoed back as `If-None-Match` on the next fetch. If the server drops the header on a later `200`, the device clears its cached value |

### HTTP 304 Not Modified

If the server responds `304 Not Modified`, the device:
- Keeps its cached ETag unchanged
- Skips the decode, dither, and e-paper refresh pipeline entirely (the e-paper retains the last rendered image without power)
- Clears `last_fetch_error`
- Reports success to the rotation scheduler

A `304` response body is ignored. The server should still honor the conditional semantics from RFC 7232 — in particular, do not send `304` when no `If-None-Match` was received.

### Config Payload Structure

The `X-Config-Payload` response header carries a JSON object matching the schemas used by `/api/config`, `/api/settings/processing`, and `/api/settings/palette`. Any top-level key may be omitted.

```json
{
  "config": { "auto_rotate": true, "rotate_cron": ["0 */12 *"], ... },
  "processing_settings": { "exposure": 1.0, ... },
  "color_palette": { "black": { "r": 2, "g": 2, "b": 2 }, ... }
}
```

---

## Error Responses

```json
{
  "status": "error",
  "message": "Error description"
}
```

Common HTTP status codes:
- `200 OK`: Success
- `400 Bad Request`: Invalid parameters
- `404 Not Found`: Resource not found
- `500 Internal Server Error`: Server error
- `503 Service Unavailable`: Device busy (display updating)
