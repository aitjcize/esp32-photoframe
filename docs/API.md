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
  "rotate_interval": 3600,
  "auto_rotate_aligned": true,
  "sleep_schedule_enabled": false,
  "sleep_schedule_start": 1380,
  "sleep_schedule_end": 420,
  "ar_mode": "interval",
  "ar_start_time": 0,
  "ar_policy": "synchronized",
  "ar_anchor": true,
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
  "deep_sleep_enabled": true
}
```

**Fields:**
- `device_name`: Device name (used for mDNS hostname)
- `timezone`: POSIX timezone string (e.g., `UTC-8` for PST)
- `ntp_server`: NTP server address
- `display_orientation`: `"landscape"` or `"portrait"`
- `display_rotation_deg`: Display rotation in degrees (0, 90, 180, 270)
- `auto_rotate`: Enable automatic image rotation
- `rotate_interval`: Rotation interval in seconds
- `auto_rotate_aligned`: Align rotation to clock boundaries
- `sleep_schedule_enabled`: Enable sleep schedule
- `sleep_schedule_start`: Sleep start time in minutes since midnight
- `sleep_schedule_end`: Sleep end time in minutes since midnight
- `ar_mode`: Advanced auto-rotate mode (`"interval"` or `"daily"`)
- `ar_start_time`: Anchor start time for advanced rotation (minutes since midnight)
- `ar_policy`: Wake-up rotation policy (`"synchronized"` to catch up, `"sequential"` to never skip)
- `ar_anchor`: Whether to use fixed time-slots for rotation
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

### `POST /api/config`

Update configuration. Only include fields to change.

**Request:**
```json
{
  "auto_rotate": true,
  "rotate_interval": 1800,
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
  "config": { "auto_rotate": true, "rotate_interval": 3600, ... },
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
