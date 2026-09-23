<script setup>
import { ref, computed, onMounted, onUnmounted } from "vue";
import { useSettingsStore, useAppStore } from "../stores";
import PaletteCalibration from "./PaletteCalibration.vue";
import GrayscaleCalibration from "./GrayscaleCalibration.vue";
import ProcessingControls from "./ProcessingControls.vue";
import RotationSchedule from "./RotationSchedule.vue";
import { isValidCron } from "../utils/cron";
import { wideEdit } from "../utils/uiPrefs";
import {
  TIMEZONE_PRESETS,
  formatDeviceWallClock,
  formatUtcOffset,
  isSimpleUtcTimezone,
  parseDeviceWallClock,
  parseUtcOffset,
  timezoneValueFromInput,
} from "../utils/timezone";

const settingsStore = useSettingsStore();
const appStore = useAppStore();

// The device rejects the entire config request when any schedule rule is
// invalid, empty or over the 7-rule budget — gate saving on the same checks.
const scheduleValid = computed(() => {
  const rules = settingsStore.deviceSettings.rotateCron || [];
  return rules.length >= 1 && rules.length <= 7 && rules.every((r) => isValidCron(r));
});

// Device time state. Prefer the wall-clock string from /api/time (already in
// the device TZ, including DST) so a CET/CEST setting is never shown as UTC.
const deviceTime = ref("");
const syncingTime = ref(false);
let deviceWallClock = null;
let wallClockSyncedAt = 0;
let tickInterval = null;

const timezonePresets = TIMEZONE_PRESETS;

const timezoneModel = computed({
  get: () => {
    const tz = settingsStore.deviceSettings.timezone || "UTC0";
    return TIMEZONE_PRESETS.find((p) => p.value === tz) || tz;
  },
  set: (value) => {
    const next = timezoneValueFromInput(value) || "UTC0";
    settingsStore.deviceSettings.timezone = next;
    settingsStore.deviceSettings.timezoneOffset = parseUtcOffset(next);
  },
});

const showUtcOffsetHelper = computed(() =>
  isSimpleUtcTimezone(settingsStore.deviceSettings.timezone)
);

const timezoneOffsetHelper = computed({
  get: () => {
    const offset = parseUtcOffset(settingsStore.deviceSettings.timezone);
    return offset === null ? "" : offset;
  },
  set: (value) => {
    if (value === "" || value === null || Number.isNaN(Number(value))) return;
    const next = formatUtcOffset(Number(value));
    settingsStore.deviceSettings.timezone = next;
    settingsStore.deviceSettings.timezoneOffset = parseUtcOffset(next);
  },
});

function updateDisplayTime() {
  if (!deviceWallClock) return;
  const elapsed = Math.floor((Date.now() - wallClockSyncedAt) / 1000);
  const current = new Date(deviceWallClock.getTime() + elapsed * 1000);
  deviceTime.value = formatDeviceWallClock(current);
}

function applyDeviceTimeResponse(data) {
  const parsed = parseDeviceWallClock(data.time);
  if (parsed) {
    deviceWallClock = parsed;
    wallClockSyncedAt = Date.now();
  }
  if (data.timezone) {
    settingsStore.deviceSettings.timezone = data.timezone;
    settingsStore.deviceSettings.timezoneOffset = parseUtcOffset(data.timezone);
  }
  updateDisplayTime();
}

async function fetchDeviceTime() {
  try {
    const response = await fetch("/api/time");
    if (response.ok) {
      applyDeviceTimeResponse(await response.json());
    }
  } catch (error) {
    console.error("Failed to fetch device time:", error);
  }
}

async function syncTime() {
  syncingTime.value = true;
  try {
    const response = await fetch("/api/time/sync", { method: "POST" });
    if (response.ok) {
      const data = await response.json();
      if (data.status === "success") {
        applyDeviceTimeResponse(data);
      }
    }
  } catch (error) {
    console.error("Failed to sync time:", error);
  } finally {
    syncingTime.value = false;
  }
}

onMounted(() => {
  fetchDeviceTime();
  loadUploadedChimes();
  // Tick every second to update display
  tickInterval = setInterval(updateDisplayTime, 1000);
});

onUnmounted(() => {
  if (tickInterval) {
    clearInterval(tickInterval);
  }
});

const tab = computed({
  get: () => settingsStore.activeSettingsTab,
  set: (val) => (settingsStore.activeSettingsTab = val),
});

const orientationOptions = computed(() => {
  const width = appStore.systemInfo.width || 800;
  const height = appStore.systemInfo.height || 480;
  const maxDim = Math.max(width, height);
  const minDim = Math.min(width, height);

  return [
    { title: `Landscape (${maxDim}×${minDim})`, value: "landscape" },
    { title: `Portrait (${minDim}×${maxDim})`, value: "portrait" },
  ];
});

// 90/270 would swap the panel's logical dimensions, which the streaming
// pipeline and dimensionless .epdgz payloads can't represent; portrait
// mounting is handled by the orientation setting instead
const rotationOptions = [
  { title: "0°", value: 0 },
  { title: "180°", value: 180 },
];

const rotationModeOptions = computed(() => {
  const options = [{ title: "URL - Fetch image from URL", value: "url" }];
  if (appStore.systemInfo.sdcard_inserted || appStore.systemInfo.has_flash_storage) {
    options.unshift({ title: "Storage - Rotate through images", value: "storage" });
  }
  return options;
});

const sdRotationModeOptions = [
  { title: "Random - Shuffle images", value: "random" },
  { title: "Sequential - In sequence", value: "sequential" },
];

const chimePresetOptions = [
  { title: "Mozart — Eine kleine Nachtmusik", value: "mozart" },
  { title: "Ode to Joy", value: "ode" },
  { title: "Frère Jacques", value: "frere" },
  { title: "Twinkle, Twinkle", value: "twinkle" },
  { title: "Fanfare", value: "fanfare" },
  { title: "Triad (C–E–G)", value: "triad" },
  { title: "Doorbell (ding-dong)", value: "dingdong" },
  { title: "Soft ping", value: "softping" },
  { title: "Alert", value: "alert" },
  { title: "Double beep", value: "doublebeep" },
];

const chimeSourceOptions = [
  { title: "Built-in preset", value: "preset" },
  { title: "WAV from URL", value: "wav" },
  { title: "Uploaded WAV", value: "uploaded" },
];

const chimePullModeOptions = [
  { title: "Once — download and cache", value: "once" },
  { title: "With each rotate — refresh then play", value: "with_rotate" },
];

const chimePlayWhenOptions = [
  { title: "Play after photo rotate (default)", value: "after" },
  { title: "Play before photo rotate", value: "before" },
];

const previewingChime = ref(false);
const pullingChime = ref(false);
const chimePreviewMessage = ref("");
const uploadedChimes = ref([]);
const uploadingChime = ref(false);
const deletingChime = ref("");
const chimeFileInput = ref(null);

function prettyChimeSize(bytes) {
  if (!bytes) return "0 B";
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${(bytes / (1024 * 1024)).toFixed(2)} MB`;
}

async function persistChimeSettings() {
  const response = await fetch("/api/config", {
    method: "PATCH",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
      chime_enabled: settingsStore.deviceSettings.chimeEnabled,
      chime_source: settingsStore.deviceSettings.chimeSource,
      chime_preset: settingsStore.deviceSettings.chimePreset,
      chime_url: settingsStore.deviceSettings.chimeUrl,
      chime_pull_mode: settingsStore.deviceSettings.chimePullMode,
      chime_play_when: settingsStore.deviceSettings.chimePlayWhen,
      chime_file: settingsStore.deviceSettings.chimeFile,
    }),
  });
  if (!response.ok) {
    const data = await response.json().catch(() => ({}));
    throw new Error(data.message || "Failed to save chime settings");
  }
}

function formatChimeApiMessage(data, fallback) {
  let msg = data.message || fallback;
  if (data.played && data.played !== "none") {
    msg += ` (${data.played})`;
  }
  if (data.error) {
    msg += `: ${data.error}`;
  }
  return msg;
}

async function loadUploadedChimes() {
  try {
    const response = await fetch("/api/chimes");
    if (!response.ok) return;
    const data = await response.json();
    uploadedChimes.value = Array.isArray(data.chimes) ? data.chimes : [];
  } catch (error) {
    console.error("Failed to list chimes:", error);
  }
}

async function uploadChimeFile(file) {
  if (!file) return;
  uploadingChime.value = true;
  chimePreviewMessage.value = "";
  try {
    const body = new FormData();
    body.append("file", file, file.name);
    const response = await fetch("/api/chime/upload", { method: "POST", body });
    const data = await response.json();
    if (!response.ok || data.status !== "success") {
      chimePreviewMessage.value = data.message || "Failed to upload chime";
      return;
    }
    settingsStore.deviceSettings.chimeSource = "uploaded";
    settingsStore.deviceSettings.chimeFile = data.filename || file.name;
    await settingsStore.loadDeviceSettings();
    await loadUploadedChimes();
    chimePreviewMessage.value = `Uploaded ${data.filename || file.name}`;
  } catch (error) {
    console.error("Failed to upload chime:", error);
    chimePreviewMessage.value = "Failed to upload chime";
  } finally {
    uploadingChime.value = false;
    if (chimeFileInput.value) chimeFileInput.value.value = "";
  }
}

function onChimeFileSelected(event) {
  const file = event.target.files?.[0];
  uploadChimeFile(file);
}

async function selectUploadedChime(name) {
  settingsStore.deviceSettings.chimeSource = "uploaded";
  settingsStore.deviceSettings.chimeFile = name;
  try {
    await fetch("/api/config", {
      method: "PATCH",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ chime_source: "uploaded", chime_file: name }),
    });
    await settingsStore.loadDeviceSettings();
  } catch (error) {
    console.error("Failed to select chime:", error);
  }
}

async function deleteUploadedChime(name) {
  deletingChime.value = name;
  try {
    const response = await fetch(`/api/chimes?name=${encodeURIComponent(name)}`, {
      method: "DELETE",
    });
    if (!response.ok) {
      const data = await response.json().catch(() => ({}));
      chimePreviewMessage.value = data.message || "Failed to delete chime";
      return;
    }
    await settingsStore.loadDeviceSettings();
    await loadUploadedChimes();
  } catch (error) {
    console.error("Failed to delete chime:", error);
    chimePreviewMessage.value = "Failed to delete chime";
  } finally {
    deletingChime.value = "";
  }
}

async function pullChimeNow() {
  pullingChime.value = true;
  chimePreviewMessage.value = "";
  try {
    await persistChimeSettings();
    const response = await fetch("/api/chime/pull", { method: "POST" });
    const data = await response.json();
    await settingsStore.loadDeviceSettings();
    if (!response.ok || data.status !== "success") {
      chimePreviewMessage.value = data.message || "Failed to pull chime";
      return;
    }
    const bytes = data.bytes ? ` (${prettyChimeSize(data.bytes)})` : "";
    chimePreviewMessage.value = `${data.message || "Chime WAV cached"}${bytes}`;
  } catch (error) {
    console.error("Failed to pull chime:", error);
    chimePreviewMessage.value = error.message || "Failed to pull chime";
  } finally {
    pullingChime.value = false;
  }
}

async function previewChime() {
  previewingChime.value = true;
  chimePreviewMessage.value = "";
  try {
    await persistChimeSettings();
    const response = await fetch("/api/chime", { method: "POST" });
    const data = await response.json();
    await settingsStore.loadDeviceSettings();
    chimePreviewMessage.value = formatChimeApiMessage(
      data,
      data.status === "success" ? "Chime played" : "Chime failed"
    );
  } catch (error) {
    console.error("Failed to preview chime:", error);
    chimePreviewMessage.value = error.message || "Failed to preview chime";
  } finally {
    previewingChime.value = false;
  }
}

const saving = ref(false);
const saveSuccess = ref(false);

function onPresetChange(preset) {
  if (preset !== "custom") {
    settingsStore.applyPreset(preset);
  }
}

function onParamsUpdate(newParams) {
  Object.assign(settingsStore.params, newParams);
}

const saveMessage = ref("");
const saveError = ref(false);

const showFactoryResetDialog = ref(false);
const resetting = ref(false);
const showImportDialog = ref(false);
const importData = ref(null);
const importFileName = ref("");

async function exportConfig() {
  try {
    const [configRes, processingRes, paletteRes] = await Promise.all([
      fetch("/api/config"),
      fetch("/api/settings/processing"),
      fetch("/api/settings/palette"),
    ]);

    const exported = {};

    if (configRes.ok) {
      const config = await configRes.json();
      // Remove sensitive fields
      delete config.wifi_password;
      exported.config = config;
    }
    if (processingRes.ok) exported.processing = await processingRes.json();
    if (paletteRes.ok) exported.palette = await paletteRes.json();

    const blob = new Blob([JSON.stringify(exported, null, 2)], { type: "application/json" });
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    const deviceName = settingsStore.deviceSettings.deviceName || "photoframe";
    a.download = `${deviceName.toLowerCase().replace(/\s+/g, "-")}-config.json`;
    a.click();
    URL.revokeObjectURL(url);
  } catch (error) {
    console.error("Failed to export config:", error);
  }
}

const downloadingLog = ref(false);

async function downloadDebugLog() {
  downloadingLog.value = true;
  try {
    const response = await fetch("/api/debug/log");
    if (!response.ok) {
      saveError.value = true;
      saveMessage.value = "No debug logs available";
      setTimeout(() => (saveError.value = false), 5000);
      return;
    }
    const blob = await response.blob();
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    const deviceName = settingsStore.deviceSettings.deviceName || "photoframe";
    a.download = `${deviceName.toLowerCase().replace(/\s+/g, "-")}-debug.log`;
    a.click();
    URL.revokeObjectURL(url);
  } catch (error) {
    console.error("Failed to download debug log:", error);
    saveError.value = true;
    saveMessage.value = "Failed to download debug logs";
    setTimeout(() => (saveError.value = false), 5000);
  } finally {
    downloadingLog.value = false;
  }
}

const clearingLog = ref(false);

async function clearDebugLog() {
  clearingLog.value = true;
  try {
    const response = await fetch("/api/debug/log", { method: "DELETE" });
    if (!response.ok) {
      throw new Error(`HTTP ${response.status}`);
    }
    saveSuccess.value = true;
    saveMessage.value = "Debug logs cleared";
    setTimeout(() => (saveSuccess.value = false), 3000);
  } catch (error) {
    console.error("Failed to clear debug logs:", error);
    saveError.value = true;
    saveMessage.value = "Failed to clear debug logs";
    setTimeout(() => (saveError.value = false), 5000);
  } finally {
    clearingLog.value = false;
  }
}

function onImportFileSelected(event) {
  const file = event.target.files?.[0];
  if (!file) return;

  importFileName.value = file.name;
  const reader = new FileReader();
  reader.onload = (e) => {
    try {
      importData.value = JSON.parse(e.target.result);
      showImportDialog.value = true;
    } catch {
      saveError.value = true;
      saveMessage.value = "Invalid JSON file";
      setTimeout(() => (saveError.value = false), 5000);
    }
  };
  reader.readAsText(file);
  // Reset input so the same file can be selected again
  event.target.value = "";
}

async function performImport() {
  if (!importData.value) return;

  showImportDialog.value = false;
  saving.value = true;

  try {
    const promises = [];

    if (importData.value.config) {
      promises.push(
        fetch("/api/config", {
          method: "PATCH",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify(importData.value.config),
        })
      );
    }
    if (importData.value.processing) {
      promises.push(
        fetch("/api/settings/processing", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify(importData.value.processing),
        })
      );
    }
    if (importData.value.palette) {
      promises.push(
        fetch("/api/settings/palette", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify(importData.value.palette),
        })
      );
    }

    await Promise.all(promises);

    // Reload all settings from device
    await Promise.all([
      settingsStore.loadDeviceSettings(),
      settingsStore.loadSettings(),
      settingsStore.loadPalette(),
    ]);

    saveSuccess.value = true;
    saveError.value = false;
    saveMessage.value = "Config imported successfully!";
    setTimeout(() => (saveSuccess.value = false), 3000);
  } catch (error) {
    console.error("Failed to import config:", error);
    saveError.value = true;
    saveMessage.value = "Failed to import config";
    setTimeout(() => (saveError.value = false), 5000);
  } finally {
    saving.value = false;
    importData.value = null;
  }
}

async function saveSettings() {
  saving.value = true;

  // Save both device settings and processing settings
  const [deviceResult, processingSuccess] = await Promise.all([
    settingsStore.saveDeviceSettings(),
    settingsStore.saveSettings(),
  ]);

  saving.value = false;

  if (deviceResult.success && processingSuccess) {
    saveSuccess.value = true;
    saveError.value = false;
    saveMessage.value = deviceResult.message || "Settings saved!";
    setTimeout(() => (saveSuccess.value = false), 3000);

    // Refresh device time in case timezone changed
    await fetchDeviceTime();
  } else {
    // Show error message
    saveError.value = true;
    saveSuccess.value = false;
    saveMessage.value = deviceResult.message || "Failed to save settings";
    setTimeout(() => (saveError.value = false), 5000);
  }
}

async function performFactoryReset() {
  resetting.value = true;
  const result = await settingsStore.factoryReset();
  resetting.value = false;
  showFactoryResetDialog.value = false;

  if (result.success) {
    saveSuccess.value = true;
    saveError.value = false;
    saveMessage.value = result.message;
    setTimeout(() => (saveSuccess.value = false), 3000);
  } else {
    saveError.value = true;
    saveSuccess.value = false;
    saveMessage.value = result.message;
    setTimeout(() => (saveError.value = false), 5000);
  }
}
</script>

<template>
  <div>
    <v-card style="overflow: visible">
      <v-card-title class="d-flex align-center">
        <v-icon icon="mdi-cog" class="mr-2" />
        Settings
      </v-card-title>

      <v-tabs v-model="tab" color="primary" show-arrows density="compact">
        <v-tab value="general"> General </v-tab>
        <v-tab value="autoRotate"> Auto Rotate </v-tab>
        <v-tab value="power"> Power </v-tab>
        <v-tab value="chimes"> Chimes </v-tab>
        <v-tab value="homeAssistant"> Home Assistant </v-tab>
        <v-tab value="processing"> Processing </v-tab>
        <v-tab value="ai"> AI Generation </v-tab>
        <v-tab value="calibration">
          {{ appStore.isGrayscale ? "Grayscale" : "Palette" }}
        </v-tab>
        <v-tab value="maintenance"> Maintenance </v-tab>
      </v-tabs>

      <v-card-text>
        <v-tabs-window v-model="tab">
          <!-- General Tab -->
          <v-tabs-window-item value="general">
            <v-row class="mt-2">
              <v-col cols="12" md="6">
                <v-text-field
                  v-model="settingsStore.deviceSettings.deviceName"
                  label="Device Name"
                  variant="outlined"
                  hint="Used for mDNS hostname (e.g., 'Living Room Frame' → living-room-frame.local)"
                  persistent-hint
                />
              </v-col>
            </v-row>

            <v-row>
              <v-col cols="12" md="6">
                <v-text-field
                  v-model="settingsStore.deviceSettings.wifiSsid"
                  label="WiFi SSID"
                  variant="outlined"
                  hint="Network name to connect to"
                  persistent-hint
                />
              </v-col>
              <v-col cols="12" md="6">
                <v-text-field
                  v-model="settingsStore.deviceSettings.wifiPassword"
                  label="WiFi Password"
                  type="password"
                  variant="outlined"
                  hint="Leave empty to keep current password"
                  persistent-hint
                  placeholder="••••••••"
                />
              </v-col>
            </v-row>

            <v-row>
              <v-col cols="12" md="6">
                <v-select
                  v-model="settingsStore.deviceSettings.displayOrientation"
                  :items="orientationOptions"
                  item-title="title"
                  item-value="value"
                  label="Display Orientation"
                  variant="outlined"
                />
              </v-col>
              <v-col cols="12" md="6">
                <v-select
                  v-model="settingsStore.deviceSettings.displayRotationDeg"
                  :items="rotationOptions"
                  item-title="title"
                  item-value="value"
                  label="Display Rotation (deg)"
                  variant="outlined"
                />
              </v-col>
            </v-row>

            <v-row>
              <v-col cols="12" md="6">
                <v-text-field
                  :model-value="deviceTime || 'Loading...'"
                  label="Device Time"
                  variant="outlined"
                  readonly
                  hint="Click sync to update from NTP server"
                  persistent-hint
                >
                  <template #append-inner>
                    <v-btn
                      icon
                      variant="text"
                      size="small"
                      :loading="syncingTime"
                      @click="syncTime"
                    >
                      <v-icon>mdi-sync</v-icon>
                      <v-tooltip activator="parent" location="top">Sync NTP</v-tooltip>
                    </v-btn>
                  </template>
                </v-text-field>
              </v-col>
              <v-col cols="12" md="6">
                <v-combobox
                  v-model="timezoneModel"
                  :items="timezonePresets"
                  item-title="title"
                  item-value="value"
                  label="Timezone (POSIX TZ)"
                  variant="outlined"
                  hint="Presets or a POSIX string (e.g. CET-1CEST,…). Saving without edits keeps the current value."
                  persistent-hint
                />
              </v-col>
            </v-row>
            <v-row v-if="showUtcOffsetHelper">
              <v-col cols="12" md="6" offset-md="6">
                <v-text-field
                  v-model.number="timezoneOffsetHelper"
                  label="UTC offset (hours)"
                  type="number"
                  :min="-12"
                  :max="14"
                  :step="0.5"
                  variant="outlined"
                  hint="ISO-style helper for fixed UTC± offsets only. POSIX UTC-1 means UTC+1."
                  persistent-hint
                />
              </v-col>
            </v-row>
            <!-- Advanced network settings (#43): collapsed by default — NTP,
                 static IP and DNS override are tinkerer territory. -->
            <v-expansion-panels class="mt-2" variant="accordion">
              <v-expansion-panel title="Advanced network settings" elevation="0">
                <v-expansion-panel-text>
                  <v-row>
                    <v-col cols="12" md="6">
                      <v-text-field
                        v-model="settingsStore.deviceSettings.ntpServer"
                        label="NTP Server"
                        variant="outlined"
                        hint="e.g., pool.ntp.org, cn.pool.ntp.org, or a local IP"
                        persistent-hint
                      />
                    </v-col>
                    <v-col cols="12" md="6">
                      <v-select
                        v-model="settingsStore.deviceSettings.ipMode"
                        :items="[
                          { title: 'Automatic (DHCP)', value: 'dhcp' },
                          { title: 'Static IP', value: 'static' },
                        ]"
                        label="IP Configuration"
                        variant="outlined"
                        hint="Applied on the next boot / wake"
                        persistent-hint
                      />
                    </v-col>
                  </v-row>
                  <v-row v-if="settingsStore.deviceSettings.ipMode === 'static'">
                    <v-col cols="12" md="4">
                      <v-text-field
                        v-model="settingsStore.deviceSettings.staticIp"
                        label="IP Address"
                        variant="outlined"
                        placeholder="192.168.1.50"
                      />
                    </v-col>
                    <v-col cols="12" md="4">
                      <v-text-field
                        v-model="settingsStore.deviceSettings.staticNetmask"
                        label="Netmask"
                        variant="outlined"
                      />
                    </v-col>
                    <v-col cols="12" md="4">
                      <v-text-field
                        v-model="settingsStore.deviceSettings.staticGateway"
                        label="Gateway"
                        variant="outlined"
                        placeholder="192.168.1.1"
                      />
                    </v-col>
                  </v-row>
                  <v-row>
                    <v-col cols="12" md="6">
                      <v-text-field
                        v-model="settingsStore.deviceSettings.dnsServer"
                        label="DNS Server"
                        variant="outlined"
                        :hint="
                          settingsStore.deviceSettings.ipMode === 'static'
                            ? 'Leave empty to use the gateway'
                            : 'Optional override; leave empty to use DHCP-provided DNS'
                        "
                        persistent-hint
                      />
                    </v-col>
                  </v-row>
                </v-expansion-panel-text>
              </v-expansion-panel>
            </v-expansion-panels>
          </v-tabs-window-item>

          <!-- Auto Rotate Tab -->
          <v-tabs-window-item value="autoRotate">
            <v-switch
              v-model="settingsStore.deviceSettings.autoRotate"
              label="Enable Auto-Rotate"
              color="primary"
              class="mb-2"
              hide-details
            />

            <div class="ml-10">
              <RotationSchedule
                v-model="settingsStore.deviceSettings.rotateCron"
                :disabled="!settingsStore.deviceSettings.autoRotate"
              />

              <v-select
                v-model="settingsStore.deviceSettings.rotationMode"
                :items="rotationModeOptions"
                item-title="title"
                item-value="value"
                label="Rotation Mode"
                variant="outlined"
                class="mt-8 mb-4"
                :disabled="!settingsStore.deviceSettings.autoRotate"
              />

              <v-expand-transition>
                <v-card
                  v-if="
                    settingsStore.deviceSettings.autoRotate &&
                    settingsStore.deviceSettings.rotationMode === 'storage'
                  "
                  variant="tonal"
                  class="mb-4"
                >
                  <v-card-text>
                    <v-select
                      v-model="settingsStore.deviceSettings.sdRotationMode"
                      :items="sdRotationModeOptions"
                      item-title="title"
                      item-value="value"
                      label="Storage Rotation Logic"
                      variant="outlined"
                      hide-details
                    />
                  </v-card-text>
                </v-card>
              </v-expand-transition>

              <v-expand-transition>
                <v-card
                  v-if="
                    settingsStore.deviceSettings.autoRotate &&
                    settingsStore.deviceSettings.rotationMode === 'url'
                  "
                  variant="tonal"
                  class="mb-4"
                >
                  <v-card-text>
                    <v-text-field
                      v-model="settingsStore.deviceSettings.imageUrl"
                      label="Image URL"
                      variant="outlined"
                      hide-details
                      class="mb-4"
                    />

                    <div
                      v-if="settingsStore.deviceSettings.caCertSet"
                      class="mb-4 d-flex flex-column ga-1"
                    >
                      <v-chip
                        color="success"
                        size="small"
                        variant="tonal"
                        style="align-self: flex-start"
                      >
                        <v-icon start>mdi-check-circle</v-icon>
                        Certificate Pinned
                      </v-chip>
                      <div class="text-caption text-medium-emphasis">
                        The TLS certificate for this HTTPS URL is pinned. It will re-pin
                        automatically when you change the URL.
                      </div>
                    </div>

                    <v-alert
                      v-if="settingsStore.deviceSettings.lastFetchError"
                      type="error"
                      variant="tonal"
                      density="compact"
                      class="mb-4"
                    >
                      Last fetch error: {{ settingsStore.deviceSettings.lastFetchError }}
                    </v-alert>

                    <v-checkbox
                      v-if="
                        appStore.systemInfo.sdcard_inserted || appStore.systemInfo.has_flash_storage
                      "
                      v-model="settingsStore.deviceSettings.saveDownloadedImages"
                      label="Save downloaded images to Downloads album"
                      color="primary"
                      class="mb-8"
                      hide-details
                    />

                    <v-text-field
                      v-model="settingsStore.deviceSettings.accessToken"
                      label="Access Token (Optional)"
                      variant="outlined"
                      hint="Sets Authorization: Bearer header"
                      persistent-hint
                      class="mt-4"
                    />

                    <v-row class="mt-4">
                      <v-col cols="12" md="6">
                        <v-text-field
                          v-model="settingsStore.deviceSettings.httpHeaderKey"
                          label="Custom Header Name"
                          variant="outlined"
                          placeholder="e.g., X-API-Key"
                        />
                      </v-col>
                      <v-col cols="12" md="6">
                        <v-text-field
                          v-model="settingsStore.deviceSettings.httpHeaderValue"
                          label="Custom Header Value"
                          variant="outlined"
                        />
                      </v-col>
                    </v-row>
                  </v-card-text>
                </v-card>
              </v-expand-transition>
            </div>
          </v-tabs-window-item>

          <!-- Power Tab -->
          <v-tabs-window-item value="power">
            <v-switch
              v-model="settingsStore.deviceSettings.deepSleepEnabled"
              label="Enable Deep Sleep"
              color="primary"
              class="mb-4"
            />

            <v-expand-transition>
              <v-alert
                v-if="!settingsStore.deviceSettings.deepSleepEnabled"
                type="warning"
                variant="tonal"
              >
                <strong>Power Consumption Notice</strong><br />
                Disabling deep sleep keeps the HTTP server accessible but significantly increases
                power consumption. Only disable if permanently powered via USB.
              </v-alert>
            </v-expand-transition>
          </v-tabs-window-item>

          <!-- Chimes Tab -->
          <v-tabs-window-item value="chimes">
            <v-alert
              v-if="!settingsStore.deviceSettings.chimeSupported"
              type="info"
              variant="tonal"
              class="mt-2"
            >
              This board has no speaker. Chime settings apply to Waveshare PhotoPainter 7.3".
            </v-alert>

            <div v-else>
              <v-switch
                v-model="settingsStore.deviceSettings.chimeEnabled"
                label="Enable speaker chime"
                color="primary"
                class="mb-2"
                hint="Master mute. Local ES8311 audio on a successful image display (timing below). Disable for battery or quiet hours."
                persistent-hint
              />

              <v-expand-transition>
                <div v-if="settingsStore.deviceSettings.chimeEnabled" class="mt-4">
                  <v-select
                    v-model="settingsStore.deviceSettings.chimeSource"
                    :items="chimeSourceOptions"
                    item-title="title"
                    item-value="value"
                    label="Chime source"
                    variant="outlined"
                    hint="Built-in tune, last WAV pulled from a URL, or a file uploaded below. Upload/select applies immediately; Pull now and Preview also save the URL fields."
                    persistent-hint
                    class="mb-4"
                  />
                  <v-select
                    v-model="settingsStore.deviceSettings.chimePreset"
                    :items="chimePresetOptions"
                    item-title="title"
                    item-value="value"
                    label="Built-in chime"
                    variant="outlined"
                    hint="Public-domain tunes synthesized on the device (about 2–8 seconds). Also used if a WAV fetch or play fails."
                    persistent-hint
                    class="mb-4"
                  />
                  <v-text-field
                    v-model="settingsStore.deviceSettings.chimeUrl"
                    label="Chime sound URL"
                    variant="outlined"
                    placeholder="http://news.local:8080/chime.wav"
                    hint="Optional PCM WAV (mono/stereo, 8–22.05 kHz, 8/16-bit, max 2 MiB / 60 seconds). When source is WAV, the frame plays the last downloaded file. Clear the URL and set source to Built-in to use a preset."
                    persistent-hint
                    class="mb-4"
                  />
                  <v-select
                    v-model="settingsStore.deviceSettings.chimePullMode"
                    :items="chimePullModeOptions"
                    item-title="title"
                    item-value="value"
                    label="WAV pull"
                    variant="outlined"
                    :disabled="settingsStore.deviceSettings.chimeSource !== 'wav'"
                    hint="Once: cache until you hit Pull now (or the first play if nothing is cached). With each rotate: GET the URL at play time (after or before the panel refresh, see below), replace the cache, then play."
                    persistent-hint
                    class="mb-4"
                  />
                  <v-select
                    v-model="settingsStore.deviceSettings.chimePlayWhen"
                    :items="chimePlayWhenOptions"
                    item-title="title"
                    item-value="value"
                    label="Play before / after photo rotate"
                    variant="outlined"
                    hint="After (default): do not start the speaker until the e-ink panel has finished drawing. Before: play when rotate starts, immediately before the panel wait."
                    persistent-hint
                    class="mb-4"
                  />
                  <v-btn
                    variant="outlined"
                    class="mb-2"
                    :loading="pullingChime"
                    :disabled="
                      settingsStore.deviceSettings.chimeSource !== 'wav' ||
                      !settingsStore.deviceSettings.chimeUrl
                    "
                    @click="pullChimeNow"
                  >
                    <v-icon start>mdi-cloud-download</v-icon>
                    Pull now
                  </v-btn>
                  <div class="text-caption text-grey mb-4">
                    Downloads the WAV from the URL, validates it, and caches it on the SD card (or
                    flash). Once mode does not fetch until you pull or the first play needs a cache.
                    Cached: {{ settingsStore.deviceSettings.chimeCached ? "yes" : "no" }}.
                  </div>

                  <div class="text-subtitle-2 mb-2">Uploaded chimes</div>
                  <div class="text-caption text-grey mb-3">
                    PCM WAV only (8–22.05 kHz, 8/16-bit, mono or stereo, max 2 MiB / 60 seconds).
                    Stored in chimes/ on the SD card (or flash). Selecting one sets the active
                    custom sound immediately.
                  </div>
                  <input
                    ref="chimeFileInput"
                    type="file"
                    accept=".wav,audio/wav,audio/x-wav"
                    hidden
                    @change="onChimeFileSelected"
                  />
                  <v-btn
                    variant="outlined"
                    class="mb-4"
                    :loading="uploadingChime"
                    @click="chimeFileInput?.click()"
                  >
                    <v-icon start>mdi-upload</v-icon>
                    Upload WAV
                  </v-btn>
                  <v-list v-if="uploadedChimes.length" class="mb-4 pa-0" density="compact">
                    <v-list-item
                      v-for="chime in uploadedChimes"
                      :key="chime.name"
                      :active="
                        settingsStore.deviceSettings.chimeSource === 'uploaded' &&
                        settingsStore.deviceSettings.chimeFile === chime.name
                      "
                      @click="selectUploadedChime(chime.name)"
                    >
                      <v-list-item-title>{{ chime.name }}</v-list-item-title>
                      <v-list-item-subtitle>{{ prettyChimeSize(chime.size) }}</v-list-item-subtitle>
                      <template #append>
                        <v-btn
                          icon="mdi-delete"
                          variant="text"
                          size="small"
                          :loading="deletingChime === chime.name"
                          @click.stop="deleteUploadedChime(chime.name)"
                        />
                      </template>
                    </v-list-item>
                  </v-list>
                  <div v-else class="text-caption text-grey mb-4">No uploaded chimes yet.</div>

                  <v-btn
                    variant="outlined"
                    :loading="previewingChime"
                    :disabled="!settingsStore.deviceSettings.chimeEnabled"
                    @click="previewChime"
                  >
                    <v-icon start>mdi-volume-high</v-icon>
                    Preview chime
                  </v-btn>
                  <div v-if="chimePreviewMessage" class="text-caption text-grey mt-2">
                    {{ chimePreviewMessage }}
                  </div>
                </div>
              </v-expand-transition>
            </div>
          </v-tabs-window-item>

          <!-- Home Assistant Tab -->
          <v-tabs-window-item class="mt-2" value="homeAssistant">
            <v-text-field
              v-model="settingsStore.deviceSettings.haUrl"
              label="Home Assistant URL"
              variant="outlined"
              placeholder="http://homeassistant.local:8123"
              hint="Configure for dynamic image serving and battery level reporting"
              persistent-hint
            />
          </v-tabs-window-item>

          <!-- Processing Tab -->
          <v-tabs-window-item value="processing">
            <div class="pa-4">
              <v-alert v-if="wideEdit" type="info" variant="tonal" density="compact">
                Processing controls are shown next to the preview in wide-edit mode. Turn wide edit
                off (the split icon on the Upload card) to edit them here.
              </v-alert>
              <ProcessingControls
                v-else
                :params="settingsStore.params"
                :preset="settingsStore.preset"
                @update:params="onParamsUpdate"
                @update:preset="settingsStore.preset = $event"
                @preset-change="onPresetChange"
              />
            </div>
          </v-tabs-window-item>

          <!-- AI Generation Tab -->
          <v-tabs-window-item value="ai">
            <v-alert type="info" variant="tonal" density="compact" class="mt-2 mb-4">
              API keys are used for client-side AI image generation when uploading images.
            </v-alert>

            <v-text-field
              v-model="settingsStore.deviceSettings.aiCredentials.openaiApiKey"
              label="OpenAI API Key"
              variant="outlined"
              type="password"
              hint="sk-..."
              persistent-hint
              class="mb-2"
            />
            <div class="text-caption text-grey ml-2 mb-4">
              Get your API key at
              <a
                href="https://platform.openai.com/api-keys"
                target="_blank"
                class="text-primary text-decoration-none"
                >platform.openai.com</a
              >
            </div>

            <v-text-field
              v-model="settingsStore.deviceSettings.aiCredentials.googleApiKey"
              label="Google Gemini API Key"
              variant="outlined"
              type="password"
              class="mb-2"
            />
            <div class="text-caption text-grey ml-2 mb-4">
              Get your API key at
              <a
                href="https://aistudio.google.com/app/apikey"
                target="_blank"
                class="text-primary text-decoration-none"
                >aistudio.google.com</a
              >
            </div>
          </v-tabs-window-item>

          <!-- Calibration Tab -->
          <v-tabs-window-item value="calibration">
            <GrayscaleCalibration v-if="appStore.isGrayscale" />
            <PaletteCalibration v-else />
          </v-tabs-window-item>

          <!-- Maintenance Tab -->
          <v-tabs-window-item value="maintenance">
            <div class="text-subtitle-1 mt-2 mb-4">Config Backup</div>
            <v-row>
              <v-col cols="12">
                <v-btn variant="outlined" class="mr-2" @click="exportConfig">
                  <v-icon start>mdi-download</v-icon>
                  Export Config
                </v-btn>
                <v-btn variant="outlined" @click="$refs.importInput.click()">
                  <v-icon start>mdi-upload</v-icon>
                  Import Config
                </v-btn>
                <input
                  ref="importInput"
                  type="file"
                  accept=".json"
                  style="display: none"
                  @change="onImportFileSelected"
                />
              </v-col>
            </v-row>

            <v-divider class="my-6" />

            <div class="text-subtitle-1 mb-4">Debug Logging</div>
            <v-row>
              <v-col cols="12">
                <v-switch
                  v-model="settingsStore.deviceSettings.debugLogEnabled"
                  label="Save console logs to storage"
                  color="primary"
                  hide-details
                  class="mb-2"
                />
                <v-expand-transition>
                  <v-alert
                    v-if="settingsStore.deviceSettings.debugLogEnabled"
                    type="info"
                    variant="tonal"
                    density="compact"
                    class="mb-4"
                  >
                    Serial console output is mirrored to the SD card, keeping only the most recent
                    lines. Takes effect after saving.
                  </v-alert>
                </v-expand-transition>
                <v-btn
                  variant="outlined"
                  class="mr-2"
                  :loading="downloadingLog"
                  @click="downloadDebugLog"
                >
                  <v-icon start>mdi-download</v-icon>
                  Download Logs
                </v-btn>
                <v-btn variant="outlined" :loading="clearingLog" @click="clearDebugLog">
                  <v-icon start>mdi-delete</v-icon>
                  Clear Logs
                </v-btn>
              </v-col>
            </v-row>

            <v-divider class="my-6" />

            <div class="text-subtitle-1 mb-4">Factory Reset</div>
            <v-row>
              <v-col cols="12">
                <v-btn color="error" variant="outlined" @click="showFactoryResetDialog = true">
                  <v-icon start>mdi-restore-alert</v-icon>
                  Factory Reset Device
                </v-btn>
              </v-col>
            </v-row>
          </v-tabs-window-item>
        </v-tabs-window>
      </v-card-text>

      <v-card-actions class="px-4 pb-4">
        <v-spacer />
        <v-fade-transition>
          <v-chip v-if="saveSuccess" color="success" variant="tonal">
            <v-icon icon="mdi-check" start />
            {{ saveMessage || "Settings saved!" }}
          </v-chip>
          <v-chip v-else-if="saveError" color="error" variant="tonal">
            <v-icon icon="mdi-alert-circle" start />
            {{ saveMessage || "Failed to save settings" }}
          </v-chip>
        </v-fade-transition>
        <v-tooltip
          text="Fix the rotation schedule first (invalid or too many rules)"
          location="top"
          :disabled="scheduleValid"
        >
          <template #activator="{ props: tooltipProps }">
            <span v-bind="tooltipProps">
              <v-btn
                color="primary"
                :loading="saving"
                :disabled="!scheduleValid"
                @click="saveSettings"
              >
                <v-icon icon="mdi-content-save" start />
                Save Settings
              </v-btn>
            </span>
          </template>
        </v-tooltip>
      </v-card-actions>
    </v-card>

    <!-- Factory Reset Confirmation Dialog -->
    <v-dialog v-model="showFactoryResetDialog" max-width="500">
      <v-card>
        <v-card-title class="text-h5 text-error">
          <v-icon icon="mdi-alert" class="mr-2" />
          Confirm Factory Reset
        </v-card-title>
        <v-card-text>
          <v-alert type="error" variant="tonal" class="mb-4">
            <div class="text-subtitle-2 mb-2">This action is irreversible!</div>
            <div class="text-body-2">
              All device settings will be permanently erased, including:
            </div>
            <ul class="mt-2">
              <li>WiFi credentials</li>
              <li>Image processing settings</li>
              <li>Device configuration</li>
              <li>All custom settings</li>
            </ul>
          </v-alert>
          <div class="text-body-1 mb-3">
            The device will restart and return to factory defaults. Are you sure you want to
            continue?
          </div>
          <v-alert type="info" variant="tonal" density="compact">
            <div class="text-body-2">
              <strong>After reset:</strong> The device will create a WiFi access point named
              <strong>"PhotoFrame"</strong>. Connect to it from your device to restart the
              provisioning process.
            </div>
          </v-alert>
        </v-card-text>
        <v-card-actions>
          <v-spacer />
          <v-btn variant="text" @click="showFactoryResetDialog = false">Cancel</v-btn>
          <v-btn color="error" variant="flat" :loading="resetting" @click="performFactoryReset">
            Reset Device
          </v-btn>
        </v-card-actions>
      </v-card>
    </v-dialog>
    <!-- Import Config Confirmation Dialog -->
    <v-dialog v-model="showImportDialog" max-width="500">
      <v-card>
        <v-card-title>
          <v-icon icon="mdi-upload" class="mr-2" />
          Import Config
        </v-card-title>
        <v-card-text>
          <v-alert type="warning" variant="tonal" class="mb-4">
            This will overwrite your current settings with the imported config.
          </v-alert>
          <div class="text-body-2 mb-2">
            File: <strong>{{ importFileName }}</strong>
          </div>
          <div v-if="importData" class="text-body-2">
            Sections to import:
            <ul class="mt-1 ml-4">
              <li v-if="importData.config">Device settings</li>
              <li v-if="importData.processing">Processing settings</li>
              <li v-if="importData.palette">Palette calibration</li>
            </ul>
          </div>
        </v-card-text>
        <v-card-actions>
          <v-spacer />
          <v-btn variant="text" @click="showImportDialog = false">Cancel</v-btn>
          <v-btn color="primary" variant="flat" @click="performImport"> Import </v-btn>
        </v-card-actions>
      </v-card>
    </v-dialog>
  </div>
</template>

<style scoped></style>
