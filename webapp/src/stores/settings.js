import { defineStore } from "pinia";
import { ref, watch } from "vue";
import { getPreset, getPresetOptions, SPECTRA6 } from "@aitjcize/epaper-image-convert";

export const useSettingsStore = defineStore("settings", () => {
  const API_BASE = "";

  // UI state
  const activeSettingsTab = ref("general");

  // Processing parameters
  const params = ref({
    exposure: 1.0,
    saturation: 1.0,
    toneMode: "contrast",
    contrast: 1.0,
    strength: 0.9,
    shadowBoost: 0.0,
    highlightCompress: 1.5,
    midpoint: 0.5,
    colorMethod: "rgb",
    ditherAlgorithm: "floyd-steinberg",
    compressDynamicRange: true,
  });

  // Device settings (UI representation)
  const deviceSettings = ref({
    deviceName: "PhotoFrame",
    displayOrientation: "landscape",
    timezoneOffset: 0,
    // Auto rotate
    autoRotate: true,
    autoRotateAligned: true,
    rotateHours: 1,
    rotateMinutes: 0,
    rotationMode: "sdcard",
    imageUrl: "https://loremflickr.com/800/480",
    saveDownloadedImages: true,
    accessToken: "",
    httpHeaderKey: "",
    httpHeaderValue: "",
    // Sleep schedule
    sleepScheduleEnabled: false,
    sleepScheduleStart: "23:00",
    sleepScheduleEnd: "07:00",
    // Power
    deepSleepEnabled: true,
    // Home Assistant
    haUrl: "",
    // Advanced
    displayRotationDeg: 180,
  });

  // Original config from server (for change detection)
  let originalConfig = {};
  let originalParams = {};

  // Palette - use defaults from epaper-image-convert library
  const palette = ref({ ...SPECTRA6.perceived });

  // Preset
  const preset = ref("balanced");

  // Get preset names from library (excluding "custom")
  const presetNames = getPresetOptions().map((p) => p.value);

  // Keys to compare for preset matching
  const presetKeys = [
    "exposure",
    "saturation",
    "toneMode",
    "contrast",
    "strength",
    "shadowBoost",
    "highlightCompress",
    "midpoint",
    "colorMethod",
    "ditherAlgorithm",
    "compressDynamicRange",
  ];

  function matchesPreset(presetName) {
    const target = getPreset(presetName);
    if (!target) return false;

    const current = params.value;
    for (const key of presetKeys) {
      // Only compare keys that exist in the target preset
      if (key in target && current[key] !== target[key]) return false;
    }
    return true;
  }

  function derivePresetFromParams() {
    for (const name of presetNames) {
      if (matchesPreset(name)) return name;
    }
    return "custom";
  }

  // Actions
  function applyPreset(presetName) {
    const presetParams = getPreset(presetName);
    if (presetParams) {
      preset.value = presetName;
      // Copy only processing params (exclude name, title, description)
      // eslint-disable-next-line no-unused-vars
      const { name, title, description, ...processingParams } = presetParams;
      Object.assign(params.value, processingParams);
    }
  }

  watch(
    params,
    () => {
      preset.value = derivePresetFromParams();
    },
    { deep: true }
  );

  async function loadSettings() {
    try {
      const response = await fetch(`${API_BASE}/api/settings/processing`);
      if (!response.ok || response.headers.get("content-type")?.includes("text/html")) {
        return;
      }
      const data = await response.json();
      Object.assign(params.value, data);
      // Store original params for change detection
      originalParams = JSON.parse(JSON.stringify(data));
    } catch (error) {
      console.log("Settings API not available (standalone mode)");
    }
  }

  async function loadDeviceSettings() {
    try {
      const response = await fetch(`${API_BASE}/api/config`);
      if (!response.ok || response.headers.get("content-type")?.includes("text/html")) {
        return;
      }
      const data = await response.json();

      // Store original config for change detection
      originalConfig = JSON.parse(JSON.stringify(data));

      // Parse config into UI-friendly format
      deviceSettings.value.autoRotate = data.auto_rotate || false;
      deviceSettings.value.autoRotateAligned = data.auto_rotate_aligned !== false;

      // Convert seconds to hours and minutes
      const rotateIntervalSeconds = data.rotate_interval || 3600;
      deviceSettings.value.rotateHours = Math.floor(rotateIntervalSeconds / 3600);
      deviceSettings.value.rotateMinutes = Math.floor((rotateIntervalSeconds % 3600) / 60);

      deviceSettings.value.displayRotationDeg = data.display_rotation_deg ?? 180;
      deviceSettings.value.imageUrl = data.image_url || "https://loremflickr.com/800/480";
      deviceSettings.value.deepSleepEnabled = data.deep_sleep_enabled !== false;
      deviceSettings.value.haUrl = data.ha_url || "";
      deviceSettings.value.saveDownloadedImages = data.save_downloaded_images !== false;
      deviceSettings.value.accessToken = data.access_token || "";
      deviceSettings.value.httpHeaderKey = data.http_header_key || "";
      deviceSettings.value.httpHeaderValue = data.http_header_value || "";
      deviceSettings.value.displayOrientation = data.display_orientation || "landscape";
      deviceSettings.value.rotationMode = data.rotation_mode || "sdcard";
      deviceSettings.value.deviceName = data.device_name || "PhotoFrame";

      // Sleep schedule
      deviceSettings.value.sleepScheduleEnabled = data.sleep_schedule_enabled || false;

      // Convert minutes to HH:MM format
      const startMinutes = data.sleep_schedule_start ?? 1380; // Default 23:00
      const startHours = Math.floor(startMinutes / 60);
      const startMins = startMinutes % 60;
      deviceSettings.value.sleepScheduleStart = `${String(startHours).padStart(2, "0")}:${String(startMins).padStart(2, "0")}`;

      const endMinutes = data.sleep_schedule_end ?? 420; // Default 07:00
      const endHours = Math.floor(endMinutes / 60);
      const endMins = endMinutes % 60;
      deviceSettings.value.sleepScheduleEnd = `${String(endHours).padStart(2, "0")}:${String(endMins).padStart(2, "0")}`;

      // Parse timezone from POSIX format (e.g., "UTC-8" -> 8)
      const timezone = data.timezone || "UTC0";
      let offset = 0;
      const match = timezone.match(/UTC([+-]?)(\d+)(?::(\d+))?/);
      if (match) {
        const sign = match[1] === "-" ? 1 : -1; // POSIX format is inverted
        const hours = parseInt(match[2]) || 0;
        const minutes = parseInt(match[3]) || 0;
        offset = sign * (hours + minutes / 60);
      }
      deviceSettings.value.timezoneOffset = offset;
    } catch (error) {
      console.log("Device settings API not available (standalone mode)");
    }
  }

  async function saveDeviceSettings() {
    // Build current config object with snake_case keys for API
    const rotateInterval =
      deviceSettings.value.rotateHours * 3600 + deviceSettings.value.rotateMinutes * 60;

    // Convert HH:MM to minutes since midnight
    const [startHours, startMins] = deviceSettings.value.sleepScheduleStart.split(":").map(Number);
    const sleepScheduleStart = startHours * 60 + startMins;

    const [endHours, endMins] = deviceSettings.value.sleepScheduleEnd.split(":").map(Number);
    const sleepScheduleEnd = endHours * 60 + endMins;

    // Convert UTC offset to POSIX timezone format
    const offsetValue = deviceSettings.value.timezoneOffset || 0;
    let timezone = "UTC0";
    if (offsetValue !== 0) {
      const absOffset = Math.abs(offsetValue);
      const hours = Math.floor(absOffset);
      const minutes = Math.round((absOffset - hours) * 60);
      const sign = offsetValue > 0 ? "-" : "+"; // Inverted for POSIX

      if (minutes === 0) {
        timezone = `UTC${sign}${hours}`;
      } else {
        timezone = `UTC${sign}${hours}:${String(minutes).padStart(2, "0")}`;
      }
    }

    const currentConfig = {
      auto_rotate: deviceSettings.value.autoRotate,
      auto_rotate_aligned: deviceSettings.value.autoRotateAligned,
      rotate_interval: rotateInterval,
      display_rotation_deg: deviceSettings.value.displayRotationDeg,
      rotation_mode: deviceSettings.value.rotationMode,
      image_url: deviceSettings.value.imageUrl,
      ha_url: deviceSettings.value.haUrl,
      deep_sleep_enabled: deviceSettings.value.deepSleepEnabled,
      save_downloaded_images: deviceSettings.value.saveDownloadedImages,
      display_orientation: deviceSettings.value.displayOrientation,
      sleep_schedule_enabled: deviceSettings.value.sleepScheduleEnabled,
      sleep_schedule_start: sleepScheduleStart,
      sleep_schedule_end: sleepScheduleEnd,
      device_name: deviceSettings.value.deviceName,
      timezone: timezone,
      access_token: deviceSettings.value.accessToken,
      http_header_key: deviceSettings.value.httpHeaderKey,
      http_header_value: deviceSettings.value.httpHeaderValue,
    };

    // Compare with original config and only send changed fields
    const changedFields = {};
    for (const key in currentConfig) {
      if (currentConfig[key] !== originalConfig[key]) {
        changedFields[key] = currentConfig[key];
      }
    }

    // If nothing changed, return success
    if (Object.keys(changedFields).length === 0) {
      return { success: true, message: "No changes to save" };
    }

    try {
      const response = await fetch(`${API_BASE}/api/config`, {
        method: "PATCH",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(changedFields),
      });
      const data = await response.json();

      if (data.status === "success") {
        // Update original config with new values
        Object.assign(originalConfig, changedFields);
        return { success: true, message: "Settings saved successfully" };
      } else {
        return { success: false, message: "Failed to save settings" };
      }
    } catch (error) {
      console.error("Error saving config:", error);
      return { success: false, message: "Error saving settings" };
    }
  }

  async function loadPalette() {
    try {
      const response = await fetch(`${API_BASE}/api/settings/palette`);
      if (!response.ok || response.headers.get("content-type")?.includes("text/html")) {
        return;
      }
      const data = await response.json();
      palette.value = data;
    } catch (error) {
      console.log("Palette API not available (standalone mode)");
    }
  }

  function hasProcessingSettingsChanged() {
    const current = params.value;
    for (const key of presetKeys) {
      if (current[key] !== originalParams[key]) {
        return true;
      }
    }
    return false;
  }

  async function saveSettings() {
    // Skip save if nothing changed
    if (!hasProcessingSettingsChanged()) {
      return true;
    }

    try {
      const response = await fetch(`${API_BASE}/api/settings/processing`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(params.value),
      });
      if (response.ok) {
        // Update original params after successful save
        originalParams = JSON.parse(JSON.stringify(params.value));
      }
      return response.ok;
    } catch (error) {
      console.error("Failed to save settings:", error);
      return false;
    }
  }

  async function savePalette() {
    try {
      const response = await fetch(`${API_BASE}/api/settings/palette`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(palette.value),
      });
      return response.ok;
    } catch (error) {
      console.error("Failed to save palette:", error);
      return false;
    }
  }

  return {
    activeSettingsTab,
    params,
    deviceSettings,
    palette,
    preset,
    presetNames,
    applyPreset,
    loadSettings,
    loadDeviceSettings,
    saveDeviceSettings,
    loadPalette,
    saveSettings,
    savePalette,
  };
});
