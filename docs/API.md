# PhotoFrame API Documentation

Complete REST API reference for the ESP32 PhotoFrame firmware.

## Base URL

All endpoints are relative to: `http://<device-ip>/`

## Endpoints

### `GET /api/images?album=<albumname>`

List all images in a specific album.

**Parameters:**
- `album`: Album name (e.g., `Vacation`)

**Example:**
```
GET /api/images?album=Vacation
```

**Response:**
```json
[
  {
    "name": "photo1.bmp"
  },
  {
    "name": "photo2.bmp"
  }
]
```

---

### `GET /api/image?name=<path>`

Serve image thumbnail (JPEG) or fallback to BMP.

**Parameters:**
- `name`: Image path in `album/filename` format (e.g., `Vacation/photo.jpg`)

**Response:** Image file (JPEG or BMP)

**Examples:**
```
GET /api/image?name=Vacation/photo.jpg
GET /api/image?name=Default/photo.jpg
```

---

### `GET /api/albums`

List all albums with their enabled status.

**Response:**
```json
[
  {
    "name": "Default",
    "enabled": true
  },
  {
    "name": "Vacation",
    "enabled": true
  },
  {
    "name": "Family",
    "enabled": false
  }
]
```

**Note:** Albums marked as `enabled: true` will be included in auto-rotation.

---

### `POST /api/albums`

Create a new album.

**Request:**
```json
{
  "name": "Vacation"
}
```

**Response:**
```json
{
  "status": "success",
  "message": "Album created"
}
```

**Note:** Creates a new directory under `/sdcard/images/<albumname>/`. Album names must be valid directory names.

---

### `DELETE /api/albums?name=<albumname>`

Delete an album and all its images.

**Parameters:**
- `name`: Album name to delete (e.g., `Vacation`)

**Example:**
```
DELETE /api/albums?name=Vacation
```

**Response:**
```json
{
  "status": "success"
}
```

**Note:** 
- Deletes the album directory and all images within it
- The "Default" album cannot be deleted
- This operation is irreversible

---

### `PUT /api/albums/enabled?name=<albumname>`

Enable or disable an album for auto-rotation.

**Parameters:**
- `name`: Album name (e.g., `Vacation`)

**Request:**
```json
{
  "enabled": true
}
```

**Example:**
```
PUT /api/albums/enabled?name=Vacation
```

**Response:**
```json
{
  "status": "success"
}
```

**Note:** Only enabled albums will be included when the device auto-rotates images.

---

### `POST /api/upload`

Upload a JPEG image (automatically converted to BMP with dithering).

**Request:** 
- Content-Type: `multipart/form-data`
- Fields (sent in this order):
  - `album`: Album name to upload to (e.g., `Vacation`, `Default`)
  - `processingMode`: Processing mode - `enhanced` (default) or `stock`
  - `image`: Full-size JPEG (800×480 or 480×800)
  - `thumbnail`: Thumbnail JPEG (200×120 or 120×200)

**Note:** Text fields (`album`, `processingMode`) must be sent **before** file fields for proper parsing.

**Processing:**
1. Client-side creates two images:
   - Full-size: 800×480 (landscape) or 480×800 (portrait) at 90% quality
   - Thumbnail: 200×120 (landscape) or 120×200 (portrait) at 85% quality
2. Upload both JPEGs to server
3. Server decodes full-size JPEG
4. Rotate portrait images 90° clockwise for display
5. Apply contrast adjustment (default 1.3x)
6. Apply brightness adjustment (default +0.3 f-stops)
7. Floyd-Steinberg dithering to 7-color palette
8. Save as BMP for display
9. Delete full-size JPEG (no longer needed)
10. Keep thumbnail JPEG for gallery (~8-12 KB)

**Response:**
```json
{
  "status": "success",
  "filename": "photo.bmp"
}
```

---

### `POST /api/display`

Display a specific image on the e-paper.

**Request:**
```json
{
  "filename": "Vacation/photo.bmp"
}
```

**Note:** Use `album/filename` format. For Default album: `"Default/photo.bmp"`

**Response (Success):**
```json
{
  "status": "success"
}
```

**Response (Busy):**
```json
{
  "status": "busy",
  "message": "Display is currently updating, please wait"
}
```
HTTP Status: `503 Service Unavailable`

**Note:** Display operation takes ~30 seconds. Concurrent requests are rejected.

---

### `POST /api/display-image`

Upload and display a JPEG image directly (for automation/integration).

**Request:** 
- Content-Type: `image/jpeg`
- Body: Raw JPEG image data (max 5MB)

**Processing:**
1. Receives raw JPEG data
2. Saves to temporary file
3. Decodes JPEG and validates dimensions
4. If portrait (height > width): rotates 90° clockwise
5. If dimensions don't match 800×480: applies cover mode scaling
   - Scales to fill entire display (maintains aspect ratio)
   - Center-crops any excess
6. Applies enhanced processing (S-curve tone mapping, dithering)
7. Displays on e-paper immediately
8. Cleans up temporary files

**Response (Success):**
```json
{
  "status": "success",
  "message": "Image displayed successfully"
}
```

**Response (Busy):**
```json
{
  "status": "busy",
  "message": "Display is currently updating, please wait"
}
```
HTTP Status: `503 Service Unavailable`

**Example Usage:**

**curl:**
```bash
curl -X POST \
  -H "Content-Type: image/jpeg" \
  --data-binary @photo.jpg \
  http://photoframe.local/api/display-image
```

**Python:**
```python
import requests

with open('photo.jpg', 'rb') as f:
    response = requests.post(
        'http://photoframe.local/api/display-image',
        data=f,
        headers={'Content-Type': 'image/jpeg'}
    )
    print(response.json())
```

**Home Assistant (REST Command):**
```yaml
rest_command:
  photoframe_display:
    url: "http://photoframe.local/api/display-image"
    method: POST
    content_type: "image/jpeg"
    payload: "{{ image_data }}"
```

**Note:** 
- This endpoint is designed for automation and integration use cases
- Image is processed with the enhanced algorithm (S-curve tone mapping, dithering)
- Display operation takes ~30 seconds
- Concurrent requests are rejected while display is busy

---

### `POST /api/delete`

Delete an image and its thumbnail.

**Request:**
```json
{
  "filename": "Vacation/photo.bmp"
}
```

**Response:**
```json
{
  "status": "success"
}
```

**Note:** 
- Use `album/filename` format. For Default album: `"Default/photo.bmp"`
- Deletes both the BMP file and corresponding JPEG thumbnail from the album directory

---

### `GET /api/config`

Get current configuration.

**Response:**
```json
{
  "rotate_interval": 3600,
  "auto_rotate": true,
  "deep_sleep_enabled": true,
  "image_url": "https://picsum.photos/800/480",
  "rotation_mode": "url"
}
```

**Fields:**
- `rotate_interval`: Rotation interval in seconds (10-86400)
- `auto_rotate`: Whether automatic image rotation is enabled
- `deep_sleep_enabled`: Whether deep sleep mode is enabled (for battery saving)
- `image_url`: URL to fetch images from (empty string if not set)
- `rotation_mode`: Image rotation mode - `"sdcard"` or `"url"`

---

### `POST /api/config`

Update configuration.

**Request:**
```json
{
  "rotate_interval": 3600,
  "auto_rotate": true,
  "deep_sleep_enabled": true,
  "image_url": "https://picsum.photos/800/480",
  "rotation_mode": "url"
}
```

**Parameters:**
- `rotate_interval`: Rotation interval in seconds (10-86400)
- `auto_rotate`: Enable/disable automatic image rotation (boolean)
- `deep_sleep_enabled`: Enable/disable deep sleep mode (boolean)
  - When enabled: Device enters deep sleep between rotations (~100µA power consumption)
  - When disabled: Device stays awake with HTTP server running (for Home Assistant integration)
- `image_url`: URL to fetch images from (string, max 256 characters)
  - Example: `"https://picsum.photos/800/480"` for random images
  - Set to empty string `""` to clear
- `rotation_mode`: Image rotation mode (string)
  - `"sdcard"`: Rotate through images on SD card
  - `"url"`: Fetch and display image from the configured URL

**Response:**
```json
{
  "status": "success"
}
```

**Notes:**
- `rotation_mode` determines which source is used for image rotation
- When `rotation_mode` is `"url"`, the device will download the image from `image_url` on each wakeup
- When `rotation_mode` is `"sdcard"`, the device rotates through enabled albums on the SD card
- The `image_url` is saved independently of `rotation_mode`, so you can switch modes without losing the URL
- Downloaded images are saved to the "Downloads" album on the SD card

---

### `GET /api/battery`

Get current battery status from AXP2101 power management IC.

**Response:**
```json
{
  "battery_voltage_mv": 4100,
  "battery_percent": 85,
  "charging": false,
  "usb_connected": true,
  "battery_connected": true
}
```

**Fields:**
- `battery_voltage_mv`: Battery voltage in millivolts (mV)
- `battery_percent`: Battery percentage 0-100
- `charging`: Whether battery is currently charging (boolean)
- `usb_connected`: Whether USB power is connected (boolean)
- `battery_connected`: Whether battery is physically connected (boolean)

**Example Usage:**

**curl:**
```bash
curl http://photoframe.local/api/battery
```

**Python:**
```python
import requests

response = requests.get('http://photoframe.local/api/battery')
battery = response.json()
print(f"Battery: {battery['battery_percent']}% ({battery['battery_voltage_mv']}mV)")
print(f"Charging: {battery['charging']}, USB: {battery['usb_connected']}")
```

**Home Assistant (REST Sensor):**
```yaml
sensor:
  - platform: rest
    name: "PhotoFrame Battery"
    resource: "http://photoframe.local/api/battery"
    method: GET
    value_template: "{{ value_json.battery_percent }}"
    unit_of_measurement: "%"
    json_attributes:
      - battery_voltage_mv
      - charging
      - usb_connected
      - battery_connected
    scan_interval: 300  # Update every 5 minutes
```

---

### `POST /api/sleep`

Trigger device to enter deep sleep mode immediately.

**Request:**
```json
{}
```

No request body required (can be empty JSON or omitted).

**Response:**
```json
{
  "status": "success",
  "message": "Entering sleep mode"
}
```

**Behavior:**
- Device will send HTTP response, then enter deep sleep after 500ms
- In deep sleep, power consumption is ~100µA
- Wake-up behavior depends on auto-rotate configuration:
  - **If auto-rotate is enabled**: Device will wake up after the configured rotation interval
  - **If auto-rotate is disabled**: Device will sleep indefinitely until manually woken
- Device can always be woken by:
  - Boot button press (GPIO 0)
  - Key button press (GPIO 21)
  - BLE wake-up (if BLE wake mode is enabled)

**Example Usage:**

**curl:**
```bash
curl -X POST http://photoframe.local/api/sleep
```

**Python:**
```python
import requests

response = requests.post('http://photoframe.local/api/sleep')
print(response.json())
```

**Home Assistant (REST Command):**
```yaml
rest_command:
  photoframe_sleep:
    url: "http://photoframe.local/api/sleep"
    method: POST

# Usage in automation:
automation:
  - alias: "PhotoFrame sleep at night"
    trigger:
      - platform: time
        at: "23:00:00"
    action:
      - service: rest_command.photoframe_sleep
```

**Note:** 
- Useful for battery conservation when photoframe is not needed
- Can be integrated with home automation to sleep during certain hours
- Device will disconnect from WiFi during sleep
- To control wake-up timing, configure auto-rotate settings via `/api/config`

---

## Image Processing Pipeline

### Client-Side (Browser)
1. **Orientation Detection**: Determine if image is portrait or landscape
2. **Cover Mode Scaling**: Scale to fill exact display dimensions
   - Landscape: 800×480 pixels
   - Portrait: 480×800 pixels
3. **JPEG Compression**: Compress to 0.9 quality
4. **Upload**: Send to server

### Server-Side (ESP32)
1. **JPEG Decoding**: Hardware-accelerated esp_jpeg decoder
2. **Portrait Rotation**: Rotate 90° clockwise if portrait (for display)
3. **Contrast Adjustment**: Apply configurable contrast (default 1.3x, pivots around 128)
4. **Brightness Adjustment**: Apply configurable f-stop adjustment (default +0.3 f-stops)
5. **Floyd-Steinberg Dithering**: Convert to 7-color palette
6. **BMP Output**: Save 800×480 BMP for display
7. **Thumbnail**: Keep original JPEG (480×800 or 800×480) for gallery

---

## Error Responses

All endpoints may return error responses in the following format:

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
- `503 Service Unavailable`: Device busy (display operation in progress)
