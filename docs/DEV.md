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

## Bounded unattended network wakes

Timer and ROTATE-button wakes that need Wi-Fi use a single monotonic budget.
The constants live in `main/network_policy.h`: network work may run for 180
seconds, with another 120 seconds reserved for image processing, panel refresh
and sleep teardown. The budget starts immediately before Wi-Fi initialization,
after the early-wake check. Local-only wakes and interactive sessions do not arm
this supervisor. Wi-Fi association **including DHCP** has its own earlier
absolute deadline (60 seconds on a scheduled wake, 30 seconds interactively,
15 seconds during provisioning), capped by the remaining network budget.

Each unattended image fetch gets one attempt. It never rotates a local fallback
on a failed fetch. A new ETag is persisted only after successful display so an
aborted download cannot falsely validate the old frame on the next wake.
Interactive fetches retain up to three attempts for transport
errors, HTTP 408/429 and 5xx; other statuses such as 401/404 are not retried within
that operation. Wrong-password/authentication disconnects stop association
retries; other disconnects retain at most five retries within the deadline.
Late IP events cannot turn an expired attempt into success. The provisioning
connection test uses the same API and keeps the AP running.

A bounded failure count lives in RTC no-init memory, without per-wake NVS
writes. Each unsuccessful network wake imposes a minimum sleep of 5, 10, 20,
40, 80, 160, 320, then 360 minutes. Sleep selects the first configured cron
boundary at or after that minimum, preserving quiet hours and longer schedules.
An early timer re-sleep does not apply that minimum again. A successful rotation,
304, valid HA veto or successful early-wake time correction clears the count.
A BOOT wake remains available for configuration; ROTATE can attempt immediately.
An unrelated reset or power loss discards the count.

### Deadline recovery and limits

HTTP socket timeouts alone do not bound a complete request: DNS, TLS, redirects
and a trickling response can overrun a per-read timeout. Requests receive the
remaining budget, streaming callbacks close expired requests, and subsequent
operations do not start after expiry. A separate priority-6 task covers blocking
calls and the rest of the pipeline. At 300 seconds it records a recovery marker
and calls `esp_restart()`. The next boot recognizes that marker and takes normal
board/storage sleep teardown before reaching cold-boot Wi-Fi or provisioning.
It does not retry the network request on the recovery boot. If the supervisor
cannot be allocated, the wake sleeps without starting Wi-Fi.

The 300-second limit is a **pipeline restart deadline**, not a measured guarantee
that the board is already asleep at that instant. Boot initialization and the
recovery boot's board/storage teardown add time. The design assumes the RTOS,
restart mechanism and board teardown are functioning; it cannot recover a dead
CPU or a hardware hang during boot. Normal failures use cooperative cleanup.
The forced-restart fallback can interrupt an image/storage operation and needs
hardware qualification, especially on SD-card boards. The two-minute display
reserve is a policy budget, not measured worst-case display timing.

OTA version checks share the network budget. Firmware installation is rejected
during a supervised wake; use a BOOT-button interactive session to install it.
HA notifications and requested config-server windows use only the remaining
network time and may be skipped/truncated after a slow update. CPU/display work
and existing background-worker ordering otherwise remain unchanged; moving the
radio-off boundary and coordinating worker teardown are separate follow-ups.

### Validation before merge

Host tests compile the production Wi-Fi manager, wake supervisor and backoff
policy against a fake event loop/clock. Run them with `make test`, or use CMake
and `ctest --test-dir host_tests/build --output-on-failure`. If `/tmp` is not
writable, set `TMPDIR` to a writable directory before running the existing image
tests. Host tests do not emulate lwIP, TLS, ESP-IDF restart, RTC retention or panel
timing.

On EE02 and at least one SD-card board, with USB disconnected:

1. Exercise absent AP, wrong password, association without DHCP, missing IP/fail
   events, marginal RSSI, DNS failure, invalid TLS certificate, HTTP 401/404/429,
   5xx, stalled headers and a body that trickles indefinitely. Verify one image
   request per unattended wake, unchanged panel content on network failure and
   eventual deep sleep. Include HA/OTA-check/thumbnail stalls.
2. Record Wi-Fi start, deadline/recovery boot and actual deep-sleep times. Force
   the supervisor path by blocking the wake task beyond 300 seconds. Verify one
   software restart, the recovery log, intact credentials, normal panel/divider
   power-off, and a backoff sleep rather than a reboot/request loop. Check SD
   integrity and partial-download cleanup. Repeat with task creation forced to
   fail in a test build.
3. Repeat failures through the six-hour cap, then restore the network and test
   200, 304 and a valid HA veto. Verify backoff clears; test frequent schedules,
   daily schedules, quiet hours and early timer wakes. Confirm ROTATE overrides
   the wait and BOOT still opens configuration.
4. Complete provisioning in DHCP and static-IP modes, with fast IP delivery,
   wrong credentials followed by corrected credentials, and intentional
   disconnect followed by a new explicit connection.
5. Exercise JPEG/PNG and display-ready images on each supported panel, measuring
   decode/refresh/teardown against the finish reserve. Check HA notifications,
   truncated config windows and the interactive OTA installation flow.

ESP-IDF target builds and these device tests are required before treating the
budget or retained recovery path as hardware-validated.
