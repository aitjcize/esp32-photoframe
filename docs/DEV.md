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

# M5Stack M5Paper v1.0/v1.1 (4.7" 16-level grayscale, ESP32 — not S3)
./build.py --board m5stack_m5paper_v11

# Clean build (optional)
./build.py --board waveshare_photopainter_73 --fullclean
```

The script automatically:
1. Builds the frontend webapp (`webapp/`)
2. Sets the correct `sdkconfig.defaults` for the selected board
3. Passes the board's target chip via `-DIDF_TARGET` (`esp32s3` for every board
   except the M5Paper, which is a plain `esp32`)
4. Runs `idf.py build` OR `idf.py build` with correct options

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

## Battery sampling validation (XIAO EE02/EE04)

These two boards take a battery ADC sample before display initialization. Timer
and ROTATE-button wakes reuse that sample for battery percentage, voltage, and
presence throughout the wake, including HTTP/HA reporting. An unsuccessful sample
stays unknown (-1); the image request continues to omit an unknown percentage.
Cold boot, BOOT-button sessions, and scheduled wakes with a detected USB host use
fresh readings. USB detection on these boards does not detect power-only wall
adapters. Other boards retain their existing sampling behavior.

The serial boot log prints `Pre-display battery sample: ... mV` (before persistent
logging starts). To qualify this change on both boards:

1. With USB power disconnected, compare repeated timer/ROTATE wakes against the
   previous firmware at comparable battery state. Capture early and previous
   loaded readings; confirm the request header and HA/API values agree with the
   early sample and existing linear percentage calculation.
2. Check the divider enable GPIO6 returns LOW after sampling, including ADC read
   failures, and remains LOW/held during deep sleep. Check a failed ADC setup or
   sample still reports unknown and does not emit a percentage header.
3. Keep a BOOT-button or cold-boot session awake while battery voltage changes;
   verify telemetry updates. Repeat with a USB host attached and while charging.
4. Verify normal refresh, HA veto, 304, config windows, and sleep entry still work.

Host tests cover the production ADC helper's cache, fresh reads, failure handling,
calibration fallback, and divider shutdown. Target builds and device measurements
are still required. This is a reduced-load sample with the MCU awake, not an
open-circuit measurement or evidence of improved battery life/fuel-gauge accuracy.
