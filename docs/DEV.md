# Developer Guide

This guide covers building the firmware from source and advanced configuration options.

## Software Requirements

- ESP-IDF v6.0 or later
- Python 3.7+ (for build tools)
- npm, vite (note: this excludes use under Wayland)
- ESP Component Manager (comes with ESP-IDF)

## Building from Source

### 1. Set up ESP-IDF

```bash
# Source the ESP-IDF environment
cd <path to esp-idf>
. ./export.sh
```

### 2. Build the Project

We provide a `build.py` helper script that handles configuration and building for different boards.

```bash
cd <path to photoframe-api>

# install npm dependencies
cd webapp
npm install
cd ..

# Build for Waveshare PhotoPainter (7.3" 7-color e-paper)
./build.py --board waveshare_photopainter_73

# Build for Seeed Studio XIAO EE02 (13.3" e-paper)
./build.py --board seeedstudio_xiao_ee02

# Build for Seeed Studio XIAO EE03 (10.3" 16-level grayscale e-paper)
./build.py --board seeedstudio_xiao_ee03

# Build for Seeed Studio XIAO EE04 (7.3" 6-color e-paper)
./build.py --board seeedstudio_xiao_ee04

# Build for Seeed Studio reTerminal E1002 (7.3" 6-color e-paper)
./build.py --board seeedstudio_reterminal_e1002

# Build for Seeed Studio reTerminal E1004 (13.3" 6-color e-paper)
./build.py --board seeedstudio_reterminal_e1004

# Clean build (optional)
./build.py --board waveshare_photopainter_73 --fullclean
```

The script automatically:
1. Builds the frontend webapp (`webapp/`)
2. Sets the correct `sdkconfig.defaults` for the selected board
3. Runs `idf.py build` OR `idf.py build` with correct options

### 3. Flash and Monitor

The project uses ESP Component Manager to automatically download the `esp_jpeg` component during the first build.

```bash
# Flash to device (replace PORT with your serial port, e.g., /dev/cu.usbserial-*)
idf.py -p PORT flash

# Monitor output
idf.py -p PORT monitor

# Flash and monitor in one go
idf.py -p PORT flash monitor
```

**Note:** On the first build, ESP-IDF will automatically download the `esp_jpeg` component from the component registry. This requires an internet connection.

## Configuration Options

Edit `main/config.h` to customize firmware behavior:

```c
#define AUTO_SLEEP_TIMEOUT_SEC      120          // Auto-sleep timeout (2 minutes)
#define DEFAULT_ROTATE_CRON         "0 */12 *"       // Default rotation schedule (every 12h)
#define DISPLAY_WIDTH               800           // E-paper width
#define DISPLAY_HEIGHT              480    // E-paper height
```

### Key Configuration Parameters

- **AUTO_SLEEP_TIMEOUT_SEC**: Time in seconds before the device enters deep sleep when idle
- **DEFAULT_ROTATE_CRON**: Default rotation schedule (cron) for fresh devices, configurable via the web interface
- **DISPLAY_WIDTH/HEIGHT**: E-paper display dimensions (800×480 for landscape)

## Development Workflow

### Serial Monitor

Monitor device logs in real-time:

```bash
idf.py -p PORT monitor
```

Press `Ctrl+]` to exit the monitor.

### Erase Flash

To completely reset the device (including WiFi credentials):

```bash
idf.py erase-flash
```

### Finding Serial Port

**macOS:**
```bash
ls /dev/cu.*
```

**Linux:**
```bash
ls /dev/ttyUSB*
```

**Windows:**
Check Device Manager for COM ports.

## Project Structure

```
esp32-photoframe/
├── main/
│   ├── main.c                 # Entry point
│   ├── config.h               # Configuration
│   ├── display_manager.c      # E-paper display control
│   ├── http_server.c          # Web server and API
│   ├── image_processor.c      # Image processing (dithering, tone mapping)
│   ├── power_manager.c        # Sleep/wake management
│   └── webapp/                # Web interface files
├── components/
│   └── epaper_src/            # E-paper driver
├── process-cli/               # Node.js CLI tool
└── docs/                      # Demo page
```

## Debugging

### Scheduled-wake task validation

Timer and ROTATE-button wakes run `deep_sleep_wake_main()` on the
`deep_sleep_wake` task with a 12288-byte stack and priority 5. Shared board,
storage and service initialization still runs in `app_main()`, which returns
immediately after handing off the wake. Its stack remains 6144 bytes. Cold
boot, BOOT-button and CLEAR-button dispatch keep their existing behavior.

If task allocation fails, the device logs the failure and uses the normal
sleep teardown without attempting rotation. A wake pipeline that unexpectedly
returns also enters sleep. If that recovery sleep call returns, the firmware
panics instead of continuing boot initialization or returning from a FreeRTOS
task. This change does not impose a deadline on existing Wi-Fi/HTTP operations.

Before flushing debug logs, sleep teardown reports
`Sleep entry: task=deep_sleep_wake stack_min=<bytes> bytes`. ESP-IDF's
high-water mark is the minimum free stack over the task's lifetime, in bytes;
the sample includes rotation and board teardown but precedes log flush and
storage unmount. The initial 12288-byte budget follows the
[independent fork's wake-task change](https://github.com/t3ste/Tlg-esp32-photoframe/commit/d8c98968cd6a3e66e0d19920630f53faca4ec21c)
and still needs measurement on this firmware. See also
[ESP-IDF stack measurement guidance](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/performance/ram-usage.html).

Run `make test` for the host dispatch tests (deferred execution, wake-source
lifetime, allocation failure and unexpected returns). Before merging, validate
on EE02 and another board using a debug build with coredumps enabled:

1. Exercise timer and ROTATE-button wakes with URL + HA + JPEG/PNG, local
   rotation, HTTP 304, HA veto, early timer wake and failed Wi-Fi. Confirm one
   rotation at most, no cold-boot Wi-Fi/provisioning initialization after the
   handoff, and eventual deep sleep. Repeat battery-powered cycles with USB
   disconnected; collect logs after reconnecting.
2. Record the minimum reported stack headroom for each case and check for
   coredumps. Use at least 2048 bytes as an initial review target, subject to
   maintainer agreement; do not describe the budget as validated before these
   measurements. Also test cold boot and BOOT/CLEAR-button wakes.
3. In a temporary test build, force the task creation result to fail. Confirm
   the allocation error, no download/rotation, and normal panel/storage sleep
   teardown. With auto-rotate enabled, confirm the next scheduled timer wake.
4. Temporarily make the wake pipeline return immediately. Confirm the logged
   recovery and deep sleep. Remove both fault injections before release.

### Enable Verbose Logging

In `idf.py menuconfig`:
1. Navigate to `Component config` → `Log output`
2. Set default log level to `Debug` or `Verbose`

### Common Issues

**Build fails with component errors:**
- Ensure ESP Component Manager is up to date
- Delete `managed_components/` and rebuild

**Flash fails:**
- Check USB cable connection
- Try a different USB port
- Reduce baud rate: `idf.py -p PORT -b 115200 flash`

**Device not responding:**
- Press and hold BOOT button while connecting USB
- Try erasing flash: `idf.py erase-flash`
