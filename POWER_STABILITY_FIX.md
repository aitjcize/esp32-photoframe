# Power Stability Fix: E-Ink Display with Dual Power Sources

## Problem Description

### Symptoms
When operating the ESP32-S3 photoframe with **both USB and battery connected simultaneously**, display corruption and system instability occurred during e-ink display refresh:
- Images displayed correctly on **USB power alone**
- Images displayed correctly on **battery power alone**
- With **both USB and battery connected**: display refresh failed with serial monitor disconnect
- Serial monitor error during display refresh: `device reports readiness to read but returned no data`
- System instability occurred at the moment of display update, **regardless of WiFi state**

### Hardware Configuration
- **MCU**: ESP32-S3
- **Display**: Waveshare 7.3" ACeP color e-paper (800x480, 6 colors)
- **PMIC**: AXP2101 (battery charging and power management)
- **Power Rails**:
  - DC1: 3.3V main rail (ESP32-S3)
  - ALDO3/ALDO4: 3.3V for SD card
  - VBUS: USB 5V input
  - Battery: 3.7V LiPo
- **Key Components**: SD card, WiFi, I2C bus shared between AXP2101, RTC, sensors

## Root Cause Analysis

The e-paper display uses an internal DC-DC booster to generate ±15-22V from 3.3V, drawing **80-150mA** during the 30-40 second refresh period.

When **both USB and battery are connected**, the AXP2101 enters **charging mode** while also powering the system. During the high-current e-paper display refresh, the simultaneous battery charging current combined with the display's high current draw causes:

1. **Power path contention** inside the AXP2101 PMIC
2. **VBUS current limit conflicts** (USB charging path + system load)
3. **System voltage fluctuations** affecting USB data lines (causing serial disconnect)
4. **Display refresh failure** due to power instability

**Key Finding**: This issue occurs ONLY when both power sources are active. It is NOT related to WiFi.

### Technical Background

**E-Paper Display Power Characteristics:**
- **Refresh Duration**: 30-40 seconds
- **Current Draw**: 80-150mA (peak during charge pump operation)
- **Internal Voltage Generation**: DC-DC booster converts 3.3V to ±15-22V

**AXP2101 PMIC Charging Characteristics:**
- **Charging Current**: Configurable, typically 500-1000mA
- **Power Path**: Simultaneous charging + system load management
- **VBUS Current Limit**: Configurable, default 500mA
- **Issue**: When charging + system load exceed safe margins, PMIC power path becomes unstable

**USB Data Line Voltage Sensitivity:**
USB 2.0 data lines (D+ and D-) operate at 3.3V logic levels. When PMIC power management becomes unstable during high-current operations, voltage fluctuations can cause the USB data line voltage to fall below the threshold required for reliable communication, causing the host to detect a disconnect event.

### Power Scenario Analysis

**Scenario 1: USB Power Only (No Battery)**
- AXP2101 in **pass-through mode**
- No battery charging current
- System load: ~190mA during display
- **Result**: STABLE ✅

**Scenario 2: Battery Power Only (No USB)**
- AXP2101 in **battery discharge mode**
- No charging activity
- System load: ~190mA during display
- **Result**: STABLE ✅

**Scenario 3: USB + Battery Connected (PROBLEM CASE)**
- AXP2101 in **charging mode**
- Battery charging current: ~500-1000mA
- System load during display: ~190mA
- **Total PMIC activity**: ~700-1200mA
- **Result**: UNSTABLE ❌ (power path contention, VBUS limit issues)

## Solution

The fix is simple and targeted: **temporarily pause battery charging during e-paper display refresh when both USB and battery are connected**.

This eliminates power path contention in the PMIC during the critical 30-40 second display refresh period, allowing the AXP2101 to operate in simple pass-through mode temporarily.

### Implementation

**Files Modified:**
1. `components/axpPower/axp_prot.h` - Added charging control function declarations
2. `components/axpPower/axp_prot.cpp` - Implemented charging control functions
3. `main/display_manager.c` - Added charging pause logic during display update

**Added to `axp_prot.h`:**
```c
// Charging control functions
void axp_disable_charging(void);
void axp_enable_charging(void);

// Diagnostic functions
int axp_get_vbus_voltage(void);
int axp_get_system_voltage(void);
const char* axp_get_charger_status_string(void);
```

**Added to `axp_prot.cpp`:**
```cpp
void axp_disable_charging(void)
{
    if (axp2101.disableCellbatteryCharge()) {
        ESP_LOGI(TAG, "Battery charging disabled");
    } else {
        ESP_LOGE(TAG, "Failed to disable battery charging");
    }
}

void axp_enable_charging(void)
{
    if (axp2101.enableCellbatteryCharge()) {
        ESP_LOGI(TAG, "Battery charging enabled");
    } else {
        ESP_LOGE(TAG, "Failed to enable battery charging");
    }
}

int axp_get_vbus_voltage(void)
{
    return axp2101.getVbusVoltage();
}

int axp_get_system_voltage(void)
{
    return axp2101.getSystemVoltage();
}

const char* axp_get_charger_status_string(void)
{
    uint8_t status = axp2101.getChargerStatus();
    switch (status) {
        case 0: return "Standby";
        case 1: return "Charging (CC)";
        case 2: return "Charging (CV)";
        case 3: return "Charge Complete";
        case 4: return "Not Charging";
        default: return "Unknown";
    }
}
```

**Modified in `display_manager.c`:**
```c
#include "axp_prot.h"  // Added to includes

esp_err_t display_manager_show_image(const char *filename)
{
    // ... existing validation and mutex acquisition ...

    // Log power diagnostics before display update
    ESP_LOGI(TAG, "=== Power Status Before Display Update ===");
    bool usb_connected = axp_is_usb_connected();
    bool battery_connected = axp_is_battery_connected();
    ESP_LOGI(TAG, "USB connected: %s", usb_connected ? "YES" : "NO");
    ESP_LOGI(TAG, "Battery connected: %s", battery_connected ? "YES" : "NO");
    ESP_LOGI(TAG, "Battery voltage: %d mV", axp_get_battery_voltage());
    ESP_LOGI(TAG, "VBUS voltage: %d mV", axp_get_vbus_voltage());
    ESP_LOGI(TAG, "System voltage: %d mV", axp_get_system_voltage());
    ESP_LOGI(TAG, "Charger status: %s", axp_get_charger_status_string());

    // Determine if we need to pause charging during display update
    // This prevents power contention when both USB and battery are connected
    bool need_charging_pause = usb_connected && battery_connected;
    if (need_charging_pause) {
        ESP_LOGI(TAG, "USB and battery both connected - pausing charging during display update");
        ESP_LOGI(TAG, "This prevents PMIC power management contention during high display current draw");
        axp_disable_charging();
        vTaskDelay(pdMS_TO_TICKS(100)); // Allow PMIC to stabilize after disabling charging
    } else {
        ESP_LOGI(TAG, "Single power source detected - no charging pause needed");
    }

    // ... existing BMP reading code ...
    
    // Error handling - restart charging if operation failed
    if (GUI_ReadBmp_RGB_6Color(filename, 0, 0) != 0) {
        ESP_LOGE(TAG, "Failed to read BMP file");
        if (need_charging_pause) {
            ESP_LOGI(TAG, "Re-enabling charging after BMP read failure");
            axp_enable_charging();
        }
        xSemaphoreGive(display_mutex);
        return ESP_FAIL;
    }

    // ... e-paper display update ...
    epaper_port_display(epd_image_buffer);

    // Re-enable charging if we disabled it
    if (need_charging_pause) {
        ESP_LOGI(TAG, "Re-enabling battery charging after display update");
        axp_enable_charging();
        ESP_LOGI(TAG, "Charger status after re-enable: %s", axp_get_charger_status_string());
    }

    // ... rest of function ...
    
    return ESP_OK;
}
```

### Key Points

1. **Check both power sources** - only pause charging if both USB and battery are connected
2. **Wait 100ms** after disabling charging to allow PMIC power path to stabilize
3. **Re-enable charging on error** - ensure charging is restored even if display operation fails
4. **Re-enable charging after success** - restore normal charging once display completes
5. **Diagnostic logging** - provides visibility into power state for debugging

## Testing and Verification

### Test Results

**Status**: ✅ **VERIFIED AND WORKING**

All test scenarios passed successfully:

1. **USB Power Only (No Battery)** ✅
   - Display operates normally
   - No charging pause triggered
   - Serial monitor remains connected

2. **Battery Power Only (No USB)** ✅
   - Display operates normally
   - No charging pause triggered
   - Serial monitor remains connected

3. **USB + Battery Connected** ✅
   - Charging automatically paused before display update
   - Display operates correctly without corruption
   - Serial monitor remains connected throughout 30-40 second refresh
   - Charging automatically resumed after display complete

### Expected Log Output

**USB + Battery (charging pause triggered):**
```
I (xxxxx) display_manager: Displaying image: /sdcard/images/example.bmp
I (xxxxx) display_manager: Pausing charging during display (USB + battery connected)
I (xxxxx) axp_prot: Battery charging disabled
I (xxxxx) display_manager: Starting e-paper display update (this takes ~30 seconds)
I (xxxxx) epaper_port: Starting display update...
... (30-40 seconds) ...
I (xxxxx) display_manager: Resuming charging after display update
I (xxxxx) axp_prot: Battery charging enabled
I (xxxxx) display_manager: Image displayed successfully
```

**USB Only or Battery Only (no charging pause):**
```
I (xxxxx) display_manager: Displaying image: /sdcard/images/example.bmp
I (xxxxx) display_manager: Starting e-paper display update (this takes ~30 seconds)
I (xxxxx) epaper_port: Starting display update...
... (30-40 seconds) ...
I (xxxxx) display_manager: Image displayed successfully
```

## Why This Works

When both USB and battery are connected, the AXP2101 PMIC must:
1. Charge the battery (500-1000mA charging current)
2. Power the system (~190mA during display)
3. Manage power path switching between VBUS and battery
4. Enforce VBUS current limits

During the e-paper display's high-current draw (80-150mA for 30-40 seconds), the combined charging + system load can exceed VBUS current limits or cause power path contention inside the PMIC, leading to:
- Voltage fluctuations on system rails
- USB data line instability (D+/D- voltage drops)
- Host detects disconnect (serial monitor connection lost)
- Display refresh failure

By **temporarily pausing battery charging** during display refresh:
- PMIC operates in simple **pass-through mode** (VBUS → system only)
- No battery charging current (eliminates 500-1000mA load)
- Total PMIC load reduced to just system consumption (~190mA)
- No power path contention
- System rails remain stable
- USB data lines maintain proper voltage levels
- Display refresh completes successfully

Battery charging automatically resumes after the 30-40 second display operation, with no user intervention required. The brief charging pause (~40 seconds max) has negligible impact on total charging time.

## Alternative Solutions Considered

### 1. Disable WiFi During Display (INCORRECT FIX)
**Approach**: Temporarily disable WiFi modem during display refresh  
**Pros**: Reduces system current by ~100-150mA  
**Cons**: Does NOT solve the actual problem  
**Verdict**: ❌ **This was the original misdiagnosis** - Testing confirmed WiFi state has no effect on the issue. The problem only occurs with USB + Battery, not WiFi.

### 2. Reduce VBUS Current Limit
**Approach**: Lower the AXP2101 VBUS current limit to prevent overload  
**Pros**: May prevent VBUS overcurrent conditions  
**Cons**: Doesn't address root cause (dual power source contention), may slow charging  
**Verdict**: Not necessary - charging pause is simpler and more effective

### 3. Hardware Capacitor Addition
**Approach**: Add 100-470µF bulk capacitance near power rails  
**Pros**: Would reduce voltage transients  
**Cons**: Requires hardware modification, doesn't address PMIC power path contention  
**Verdict**: Not necessary - software fix was sufficient

### 4. Increase Battery Capacity / Lower ESR Battery
**Approach**: Use larger or lower internal resistance battery  
**Pros**: Can handle higher current draw  
**Cons**: Doesn't address PMIC charging + load contention  
**Verdict**: Not necessary - software fix was sufficient

## Lessons Learned

1. **Accurate Problem Diagnosis is Critical**: The original fix (disable WiFi) was based on incorrect root cause analysis. Testing all power configurations (USB only, battery only, USB + battery) revealed the actual issue.

2. **Dual Power Source Complexity**: When both USB and battery are connected, PMICs have complex power management tasks (charging, power path switching, current limiting) that can conflict with high-current system loads.

3. **Simple Fixes Are Best**: Pausing charging during display is a 10-line fix with no user impact, much simpler than hardware modifications or complex power management schemes.

4. **Diagnostic Logging**: Power diagnostics (voltage, current, charger status) are invaluable for understanding system behavior and verifying fixes.

5. **Test All Scenarios**: Always test all power configurations independently to isolate variables.

## Impact Analysis

### User Experience
- **Charging Pause**: 30-40 seconds during image refresh when both USB and battery connected
- **Charging Resume**: Automatic, no user intervention required
- **No Manual Intervention**: Process is completely transparent to the user
- **No Display Quality Impact**: Display quality and speed unchanged

### System Reliability
- **Dual Power Source Operation**: Now works reliably with both USB and battery connected
- **USB Stability**: Serial monitor remains connected during all operations
- **Display Quality**: No corruption or instability during refresh
- **Long-term Stability**: No progressive degradation over multiple refresh cycles

## Related Issues

- If charging pause is insufficient: Consider also reducing VBUS current limit via `axp_set_vbus_current_limit()`
- If issue persists on USB-only or battery-only: Different root cause (not PMIC charging contention)
- If other high-current operations cause instability with USB + battery: Apply similar charging pause approach

## References

- AXP2101 Datasheet - Power Path Management
- ESP32-S3 Technical Reference Manual - Power Management
- USB 2.0 Specification - Data Line Voltage Levels
- Waveshare 7.3" ACeP E-Paper Specification

## Change History

- **2026-01-07**: ✅ **Issue Resolved** - Corrected implementation with charging pause approach
  - **Problem**: Correctly identified as USB + Battery dual power source issue
  - **Fix**: Pause battery charging during display when both power sources connected
  - **Result**: Complete solution verified and tested successfully
  - **Files Changed**: 3 (`axp_prot.h`, `axp_prot.cpp`, `display_manager.c`)
  - **Testing**: All power configurations tested and working correctly

---

**Status**: ✅ **RESOLVED AND VERIFIED**  
**Impact**: Critical - Enables reliable operation with USB + Battery connected  
**Complexity**: Low - Simple charging pause/resume during display  
**Files Changed**: 3 (`components/axpPower/axp_prot.h`, `components/axpPower/axp_prot.cpp`, `main/display_manager.c`)  
**Affected Hardware**: All ESP32-S3 photoframes using both USB and battery power simultaneously
