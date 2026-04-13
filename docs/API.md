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
  "width": 800,
  "height": 480,
  "sdcard_inserted": true,
  "has_flash_storage": true,
  "free_heap": 1234567,
  "uptime": 3600
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

Format the storage filesystem.

### `POST /api/factory-reset`

Factory reset all settings to defaults.

---

## Request Headers (Image Fetch)

When the device fetches images from a URL, it sends these headers:

| Header | Description |
|--------|-------------|
| `X-Hostname` | Device mDNS hostname |
| `X-Display-Width` | Native panel width (e.g., 800) |
| `X-Display-Height` | Native panel height (e.g., 480) |
| `X-Display-Orientation` | `landscape` or `portrait` |
| `X-Firmware-Version` | Firmware version string |
| `X-Config-Last-Updated` | Config sync timestamp |
| `X-Processing-Settings` | JSON processing parameters |
| `X-Color-Palette` | JSON color palette |

## Response Headers (Image Fetch)

The server can include these headers in image responses:

| Header | Description |
|--------|-------------|
| `X-Thumbnail-URL` | URL to download a thumbnail |
| `X-Config-Payload` | JSON config to sync to device |

### Config Payload Structure

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
