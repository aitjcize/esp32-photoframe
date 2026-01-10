# Remote Gallery Feature Implementation Plan

## 1. Overview
The "Remote Gallery" feature allows the ESP32 PhotoFrame to fetch a pre-processed image from a remote web server and display it on the e-paper screen. This functionality is an alternative to the existing local SD card gallery mode.

### Key Characteristics
- **Source:** Images are fetched via HTTPS GET from `https://frame.svetuskin.com/image`.
- **Format:** The server provides a pre-processed `.bmp` file (800x480, 7-color dithered), requiring no on-device processing.
- **Selection:** The user selects between "Local Gallery" (SD Card) and "Remote Gallery" modes via the web interface.
- **Fallback:** If the remote fetch fails (e.g., network error), the system falls back to the local gallery to ensure the screen always updates.
- **Architecture:** The application logic (`main.c` / `power_manager.c`) acts as the orchestrator, handling data acquisition (downloading) and resource management (WiFi). The `display_manager` focuses purely on presentation logic, unaware of the download process.

## 2. Configuration & Data Structures

### 2.1 Hardcoded Configuration
The remote URL will be defined as a constant in `main/config.h`:
```c
#define REMOTE_GALLERY_URL "https://frame.svetuskin.com/image"
#define TEMP_IMAGE_PATH "/sdcard/remote.bmp"
```

### 2.2 NVS Storage
A new key will be used to persist the user's mode selection in the `photoframe` namespace.
- **Key:** `display_mode`
- **Type:** Integer (`uint8_t`)
- **Values:**
  - `0`: Local Gallery (Default)
  - `1`: Remote Gallery

## 3. Implementation Modules

### 3.1 Remote Gallery Module (`main/remote_gallery/`)
A dedicated module to handle the HTTP operations.

**Files:**
- `main/remote_gallery/remote_gallery.c`
- `main/remote_gallery/remote_gallery.h`

**Core Function:**
```c
/**
 * @brief Download a file from a URL to the SD card
 * 
 * @param url The URL to fetch (e.g., REMOTE_GALLERY_URL)
 * @param target_path Full path on SD card to save file
 * @return esp_err_t ESP_OK on success, ESP_FAIL on network/write error
 */
esp_err_t remote_gallery_download_file(const char *url, const char *target_path);
```

### 3.2 Display Manager (`main/display_manager.c`)
The `display_manager` receives a "display command" via its entry point.

**Refactored Entry Point:**
- **Function:** `void display_manager_handle_wakeup(uint8_t gallery_mode)`
- **Logic:**
  1. **If Remote Mode (gallery_mode == 1):**
     - Check if `/sdcard/remote.bmp` exists.
     - If yes: Call `display_manager_show_image("/sdcard/remote.bmp")`.
     - If no: Fallback to Local Mode (log warning).
  2. **If Local Mode (gallery_mode == 0):**
     - Select random image from enabled SD card albums.
     - Call `display_manager_show_image(random_image_path)`.

*Note: The display manager does NOT read NVS for gallery mode. It relies on the caller to provide it.*

### 3.3 Application Orchestration (The "Business Logic")
The callers are responsible for the entire "Download -> Disconnect -> Display" sequence.

#### A. Battery Mode (`main.c`)
In `app_main`, during the wakeup check:
1. Read `display_mode` from NVS.
2. **If Remote Mode:**
   - **Init & Connect WiFi.**
   - Call `remote_gallery_download_file(REMOTE_GALLERY_URL, TEMP_IMAGE_PATH)`.
   - **Disconnect & Deinit WiFi.** (Critical step to save power before the long display update).
   - If Download Success:
     - Call `display_manager_handle_wakeup(1)` (Remote Mode).
   - If Download Fail:
     - Call `display_manager_handle_wakeup(0)` (Force Local Mode).
3. **If Local Mode:**
   - Call `display_manager_handle_wakeup(0)` (Local Mode).
4. Enter Deep Sleep.

#### B. USB / Always-On Mode (`power_manager.c`)
In `rotation_timer_task`:
1. Read `display_mode` from NVS.
2. **If Remote Mode:**
   - (WiFi is already connected).
   - Call `remote_gallery_download_file(...)`.
   - If Success:
     - Call `display_manager_handle_wakeup(1)` (Remote Mode).
   - If Fail:
     - Call `display_manager_handle_wakeup(0)` (Force Local Mode).
3. **If Local Mode:**
   - Call `display_manager_handle_wakeup(0)` (Local Mode).

### 3.4 HTTP API Updates (`main/http_server.c`)
- **GET/POST /api/config**: Add support for `display_mode` (0/1).

### 3.5 Web Frontend Updates (`main/webapp/`)
- **UI:** Add "Gallery Mode" radio buttons (Local vs Remote).
- **JS:** Handle loading and saving the new setting.

## 4. Build System
- Add `remote_gallery.c` to `main/CMakeLists.txt`.
- Ensure `esp_http_client` component is linked.
