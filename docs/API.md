# PhotoFrame API Documentation

Complete REST API reference for the ESP32 PhotoFrame firmware.

## Base URL

All endpoints are relative to: `http://<device-ip>/`

## Endpoints

### `GET /api/images`

List all images on the SD card with thumbnails.

**Response:**
```json
{
  "images": [
    {
      "name": "photo.bmp",
      "size": 1152000
    }
  ]
}
```

---

### `GET /api/image?name=<filename>`

Serve image thumbnail (JPEG) or fallback to BMP.

**Parameters:**
- `name`: Image filename (e.g., `photo.jpg`)

**Response:** Image file (JPEG or BMP)

---

### `POST /api/upload`

Upload a JPEG image (automatically converted to BMP with dithering).

**Request:** 
- Content-Type: `multipart/form-data`
- Fields:
  - `image`: Full-size JPEG (800×480 or 480×800)
  - `thumbnail`: Thumbnail JPEG (200×120 or 120×200)

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
  "filename": "photo.bmp"
}
```

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

**Note:** Display operation takes ~30-40 seconds. Concurrent requests are rejected.

---

### `POST /api/delete`

Delete an image and its thumbnail.

**Request:**
```json
{
  "filename": "photo.bmp"
}
```

**Response:**
```json
{
  "status": "success"
}
```

**Note:** Deletes both the BMP file and corresponding JPEG thumbnail.

---

### `GET /api/config`

Get current configuration.

**Response:**
```json
{
  "rotate_interval": 60,
  "auto_rotate": false,
  "brightness_fstop": 0.3,
  "contrast": 1.3
}
```

---

### `POST /api/config`

Update configuration.

**Request:**
```json
{
  "rotate_interval": 120,
  "auto_rotate": true,
  "brightness_fstop": 0.5,
  "contrast": 1.5
}
```

**Parameters:**
- `rotate_interval`: Rotation interval in seconds (10-3600)
- `auto_rotate`: Enable/disable automatic image rotation
- `brightness_fstop`: Brightness adjustment in f-stops (-2.0 to 2.0)
- `contrast`: Contrast multiplier (0.5 to 2.0, 1.0 = normal)

**Response:**
```json
{
  "status": "success"
}
```

---

### `GET /api/battery`

Get current battery status.

**Response:**
```json
{
  "connected": true,
  "percent": 85,
  "voltage": 3950,
  "charging": false
}
```

**Fields:**
- `connected`: Whether battery is connected (boolean)
- `percent`: Battery percentage 0-100, or -1 if not connected
- `voltage`: Battery voltage in millivolts (mV)
- `charging`: Whether battery is currently charging (boolean)

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
