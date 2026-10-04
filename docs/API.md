# PhotoFrame API Documentation

Complete REST API reference for the ESP32 PhotoFrame firmware.

## Base URL

All endpoints are relative to: `http://<device-ip>/`

## Authentication

Off by default. When a device password is set (`http_password` via
`PATCH /api/config`, or **Settings → General → Advanced network settings** in
the web UI), every route is gated — the API, `/` and the web UI's assets alike.
`GET /api/config` reports `http_auth_enabled: true` while one is set; the
password itself is never returned.

The scheme is HTTP Basic. The username is ignored: the password is the whole
credential, so any `user:password` pair with the right password is accepted.

```bash
curl -u ":secret" http://photoframe.local/api/system-info
```

A request without a valid credential gets `401 Unauthorized` with
`WWW-Authenticate: Basic realm="ESP32 PhotoFrame"` and the body
`{"error":"authentication required"}`.

Wrong passwords are rate-limited per client address: 5 wrong passwords are
free, the next one locks the client out for 30 s, and each further wrong
password doubles the lockout up to 15 minutes. While locked out the client gets
`429 Too Many Requests` with a `Retry-After` header (seconds) and the body
`{"error":"too many wrong passwords, try again later"}` — its password is not
even checked until the lockout expires. A correct password clears the client's
record, and changing the device password resets every lockout. A request with
no `Authorization` header at all (a browser's first request before it shows its
prompt) answers `401` without counting as a wrong password.

Note that Basic credentials travel base64-encoded, not encrypted: over plain
HTTP anyone on the same network can read and replay them. The password raises
the bar against casual access on a shared LAN; it is not a defence against
someone who can already sniff your traffic.

There is no way back in over the network: `POST /api/factory-reset` sits
behind the gate like everything else. A forgotten password is cleared by
erasing the settings (NVS) partition over USB, which resets every other
setting too — see the README.

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
  "storage_used": 1048576,
  "last_crash": null
}
```

`last_crash` is `null` until the frame recovers from a crash, then describes
the newest one (see [Decoding a crash report](DEV.md#decoding-a-crash-report)):

```json
{
  "summary": "LoadProhibited, vaddr 0x00000000 | task httpd, pc 0x4201a2b3, bt 0x4201a2b3 0x4201c3d4 | fw v2.20.1, elf 1a2b3c4d, board seeedstudio_xiao_ee02 | found 2026-09-21T14:13:20Z | dump 23456 B",
  "reason": "LoadProhibited, vaddr 0x00000000",
  "task": "httpd",
  "pc": "0x4201a2b3",
  "backtrace": ["0x4201a2b3", "0x4201c3d4"],
  "backtrace_corrupted": false,
  "firmware": "v2.20.1",
  "elf_sha256": "1a2b3c4d",
  "time": 1790000000,
  "dump_size": 23456
}
```

`time` is when the next boot found the crash (Unix seconds), `null` if the
clock wasn't set. `firmware` is `null` when the crash came from a different
build than the one running; `elf_sha256` (the first 8 hex digits of the
crashed build's ELF SHA-256) still identifies it, unless the dump did not
record one, in which case it is `null` too.

### `POST /api/crash/clear`

Forget the `last_crash` record.

**Response:**
```json
{
  "status": "success"
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
  "timezone": "PST8PDT,M3.2.0,M11.1.0",
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
  "http_auth_enabled": false,
  "http_header_key": "",
  "http_header_value": "",
  "save_downloaded_images": true,
  "ha_url": "",
  "openai_api_key": "",
  "google_api_key": "",
  "deep_sleep_enabled": true
}
```

**Fields:**
- `device_name`: Device name (used for mDNS hostname)
- `timezone`: POSIX TZ string, applied with `tzset()`. A fixed offset is
  `UTC±H[:MM]` with the POSIX sign, which is inverted from everyday notation:
  `UTC-8` is eight hours *ahead* of UTC (Taipei), `UTC+5` five hours behind.
  A zone that observes DST needs the full POSIX rule so hours-based schedules
  follow the changeover, e.g. `PST8PDT,M3.2.0,M11.1.0` for US Pacific or
  `CET-1CEST,M3.5.0,M10.5.0/3` for Central Europe. The web UI's time zone
  picker fills this in from an IANA zone name. Must be 1–63 printable ASCII
  characters; the rule itself is not parsed, and one `tzset()` cannot read
  silently behaves as UTC.
- `ntp_server`: NTP server address
- `display_orientation`: `"landscape"` or `"portrait"`
- `display_rotation_deg`: Display rotation in degrees (0, 90, 180, 270)
- `auto_rotate`: Enable automatic image rotation
- `rotate_cron`: Array of 1 to 7 simplified 3-field cron expressions
  (`minute hour day-of-week`), evaluated in the device's `timezone`. The next
  rotation is the earliest time matching any rule. Supports `*`, `a`, `a-b`, `*/n`, `a-b/n` and comma lists; day-of-week
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
- `http_auth_enabled`: Whether a device password is set (see
  [Authentication](#authentication)). Read-only; the password itself is never
  returned — set or clear it with `http_password` on `PATCH /api/config`
- `http_header_key`/`http_header_value`: Custom HTTP header for image fetches
- `save_downloaded_images`: Save fetched images to Downloads album
- `ha_url`: Home Assistant URL for integration
- `openai_api_key`/`google_api_key`: AI API keys for client-side generation
- `deep_sleep_enabled`: Enable deep sleep between rotations

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

**Device password:** `http_password` sets the password that gates the whole
HTTP API (see [Authentication](#authentication)). A string sets it, `""` clears
it (authentication off), and JSON `null` or an absent key leaves it unchanged.
It must be at most 63 bytes; a longer one is refused with `400` and
`"Device password is too long (max 63 bytes)"`, nothing truncated. Changing or
clearing it resets any brute-force lockouts. It takes effect immediately: the
next request must already carry the new password. The key is ignored when it
arrives in a server-pushed config (see
[Config Payload Structure](#config-payload-structure)).

### `PATCH /api/config`

Same as `POST /api/config`. Both methods accept partial updates.

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

Trigger image rotation (respects rotation mode). Synchronous: the response is
sent once the panel has been updated.

**Response:**
```json
{ "status": "success", "message": "Image rotation triggered" }
```

In URL mode a fetch that fails leaves the current picture on the panel (there
is no fallback to a local rotation) and answers `502 Bad Gateway` with
`{ "status": "error", "message": "<reason>" }`; the same reason is reported as
`last_fetch_error` by `GET /api/config`.

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

Factory reset all settings to defaults.

---

## URL Rotation Fetch

When `rotation_mode` is `"url"`, the device issues an HTTP `GET` against `image_url` on every rotation. This section describes that request — the headers sent, what the server can send back, and the caching / config-sync protocol.

**Transport:**
- Method: `GET`
- Timeout: 30 s per socket operation (connect, time to first byte, and every wait for more data), so a dead link fails fast while a slow transfer on weak WiFi may still take a minute or more
- Retries: up to 3 attempts with a 3 s delay between them, but a retry only starts while the fetch has used less than 20 s in total — quick failures (connection refused, DNS, a server hiccup, zero-length body) are retried, an attempt that already ran into the I/O timeout is not. A completed `4xx` response is never retried: it is the server's verdict on the request (bad URL, bad token, `429`), while `5xx` and transport errors — including a `4xx` whose body was cut off — get their retries
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
| `X-Battery-Percentage` | Battery level `0`–`100`, sent whenever the board reports a percentage in that range and omitted otherwise (a board with no battery gauge reports none), so absence means "unknown" rather than `0`. A frame running without a battery may still send `0` if its gauge reads the empty rail that way |

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
| `ETag` | Opaque validator cached by the device and echoed back as `If-None-Match` on the next fetch. It is persisted only once the picture is on the panel — after a failed decode or display the previous ETag stays, so the next fetch asks for this image again instead of getting a `304` for one it never showed — and only if a config push in the same response did not change `image_url` (the new URL starts without one). If the server drops the header on a later `200`, the device clears its cached value |

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

`http_password` is ignored in a pushed `config` (the device logs a warning and
applies the rest): the push rides on the frame's own outbound request, with no
credential check, so a compromised or misconfigured server must not be able to
lock the owner out or quietly open the device.

---

## Error Responses

Which body an error carries depends on what failed, not only on which
endpoint was called, so pick the parser from the response's `Content-Type`
(JSON bodies are sent as `application/json`), never from the route alone.

Failures an endpoint diagnoses itself — a rejected config value, a failed
fetch, a certificate that could not be pinned, a reset that could not erase
NVS — come back on `/api/config`, `/api/rotate` and `/api/factory-reset` as:

```json
{
  "status": "error",
  "message": "Error description"
}
```

Request-level rejections — a body over the size limit, malformed JSON, a
missing multipart boundary, a request while the device is still initializing
— are answered with the message as plain text on every endpoint, those three
included. Image upload, delete and album operations report all their failures
as plain text, and `/api/format-storage` is mixed (its unsupported-storage
`400` is plain text, a failed format is JSON). The display endpoints
(`/api/display`, `/api/display-image`) report a panel refresh already in
progress as `503` with JSON of their own,
`{"status":"busy","message":"Display is currently updating, please wait"}`,
and other failures as plain text.

The authentication gate (see [Authentication](#authentication)) answers before
any endpoint runs and uses a different shape:

```json
{
  "error": "authentication required"
}
```

Common HTTP status codes:
- `200 OK`: Success
- `400 Bad Request`: Invalid parameters
- `401 Unauthorized`: A device password is set and the request carried no valid credential (`WWW-Authenticate: Basic realm="ESP32 PhotoFrame"`)
- `404 Not Found`: Resource not found
- `429 Too Many Requests`: Too many wrong passwords from this client; `Retry-After` says how many seconds to wait
- `500 Internal Server Error`: Server error
- `502 Bad Gateway`: `POST /api/rotate` in URL mode could not fetch the image (the panel is left unchanged)
- `503 Service Unavailable`: Device busy (display updating)
