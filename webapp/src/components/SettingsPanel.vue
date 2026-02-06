<script setup>
import { ref, computed, onMounted, onUnmounted } from "vue";
import { getDitherOptions, getPresetOptions } from "@aitjcize/epaper-image-convert";
import { useSettingsStore, useAppStore } from "../stores";
import PaletteCalibration from "./PaletteCalibration.vue";

const settingsStore = useSettingsStore();
const appStore = useAppStore();

// Device time state
const deviceTime = ref("");
const syncingTime = ref(false);
let deviceTimestamp = null; // Unix timestamp from device
let localTimeOffset = 0; // Offset between device time and local time
let tickInterval = null;

function updateDisplayTime() {
  if (deviceTimestamp === null) return;
  // Calculate current device time based on elapsed local time
  const elapsed = Math.floor((Date.now() - localTimeOffset) / 1000);
  const currentTimestamp = deviceTimestamp + elapsed;

  // Apply timezone offset for display
  // We shift the timestamp by the offset so that toISOString() (which is UTC)
  // displays the correct local time numbers.
  const offsetHours = settingsStore.deviceSettings.timezoneOffset || 0;
  const adjustedTimestamp = currentTimestamp + offsetHours * 3600;

  const date = new Date(adjustedTimestamp * 1000);
  // Format as YYYY-MM-DD HH:MM:SS
  deviceTime.value = date.toISOString().slice(0, 19).replace("T", " ");
}

async function parseTimezone(timezoneStr) {
  if (!timezoneStr) return;

  // Posix format: UTC[+/-]H[:MM] (e.g., UTC-8 or UTC+5:30)
  // Note: POSIX sign is inverted relative to ISO8601
  let offset = 0;
  const match = timezoneStr.match(/UTC([+-]?)(\d+)(?::(\d+))?/);
  if (match) {
    const sign = match[1] === "-" ? 1 : -1; // POSIX Inverted
    const hours = parseInt(match[2]) || 0;
    const minutes = parseInt(match[3]) || 0;
    offset = sign * (hours + minutes / 60);

    // Update store if different, to keep UI in sync
    if (settingsStore.deviceSettings.timezoneOffset !== offset) {
      settingsStore.deviceSettings.timezoneOffset = offset;
    }
  }
}

async function fetchDeviceTime() {
  try {
    const response = await fetch("/api/time");
    if (response.ok) {
      const data = await response.json();
      deviceTimestamp = data.timestamp;
      localTimeOffset = Date.now();
      await parseTimezone(data.timezone);
      updateDisplayTime();
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
        deviceTimestamp = data.timestamp;
        localTimeOffset = Date.now();
        await parseTimezone(data.timezone);
        updateDisplayTime();
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

const rotationOptions = [
  { title: "0°", value: 0 },
  { title: "90°", value: 90 },
  { title: "180°", value: 180 },
  { title: "270°", value: 270 },
];

const rotationModeOptions = computed(() => {
  const options = [
    { title: "URL - Fetch image from URL", value: "url" },
    { title: "AI - Generate using AI", value: "ai" },
  ];
  if (appStore.systemInfo.sdcard_inserted) {
    options.unshift({ title: "SD Card - Rotate through images", value: "sdcard" });
  }
  return options;
});

const sdRotationModeOptions = [
  { title: "Random - Shuffle images", value: "random" },
  { title: "Sequential - In sequence", value: "sequential" },
];

const saving = ref(false);
const saveSuccess = ref(false);

const presetOptions = [
  ...getPresetOptions(),
  { value: "custom", title: "Custom", description: "Manually adjusted parameters" },
];

const toneModeOptions = [
  { title: "Contrast", value: "contrast" },
  { title: "S-Curve", value: "scurve" },
];

const colorMethodOptions = [
  { title: "Simple RGB", value: "rgb" },
  { title: "LAB Color Space", value: "lab" },
];

const aiProviderOptions = [{ title: "OpenAI", value: 0 }];

const aiModelOptions = [
  { title: "GPT Image 1.5", value: "gpt-image-1.5" },
  { title: "GPT Image 1", value: "gpt-image-1" },
  { title: "GPT Image 1 Mini", value: "gpt-image-1-mini" },
];

const ditherOptions = getDitherOptions();

const isScurveMode = computed(() => settingsStore.params.toneMode === "scurve");

function onPresetChange(preset) {
  if (preset !== "custom") {
    settingsStore.applyPreset(preset);
  }
}

const saveMessage = ref("");
const saveError = ref(false);
const showFactoryResetDialog = ref(false);
const resetting = ref(false);

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
        <v-tab value="homeAssistant"> Home Assistant </v-tab>
        <v-tab value="processing"> Processing </v-tab>
        <v-tab value="ai"> AI Generation </v-tab>
        <v-tab value="calibration"> Palette Calibration </v-tab>
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
                <v-text-field
                  v-model.number="settingsStore.deviceSettings.timezoneOffset"
                  label="Timezone (UTC offset)"
                  type="number"
                  :min="-12"
                  :max="14"
                  :step="0.5"
                  variant="outlined"
                  hint="e.g., -8 for PST, +1 for CET, +8 for CST"
                  persistent-hint
                />
              </v-col>
            </v-row>

            <!-- Factory Reset Section -->
            <v-divider class="my-6" />
            <v-row>
              <v-col cols="12">
                <v-btn color="error" variant="outlined" @click="showFactoryResetDialog = true">
                  <v-icon start>mdi-restore-alert</v-icon>
                  Factory Reset Device
                </v-btn>
              </v-col>
            </v-row>
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
              <v-row class="mb-0">
                <v-col cols="6" md="3">
                  <v-text-field
                    v-model.number="settingsStore.deviceSettings.rotateHours"
                    label="Hours"
                    type="number"
                    :min="0"
                    :max="23"
                    variant="outlined"
                    :disabled="!settingsStore.deviceSettings.autoRotate"
                    hide-details
                  />
                </v-col>
                <v-col cols="6" md="3">
                  <v-text-field
                    v-model.number="settingsStore.deviceSettings.rotateMinutes"
                    label="Minutes"
                    type="number"
                    :min="0"
                    :max="59"
                    variant="outlined"
                    :disabled="!settingsStore.deviceSettings.autoRotate"
                    hide-details
                  />
                </v-col>
              </v-row>

              <v-checkbox
                v-model="settingsStore.deviceSettings.autoRotateAligned"
                label="Align rotation to clock boundaries"
                hide-details
                class="mb-0 ml-2"
                :disabled="!settingsStore.deviceSettings.autoRotate"
              />
              <v-alert
                v-if="settingsStore.deviceSettings.autoRotateAligned"
                type="info"
                variant="tonal"
                density="compact"
              >
                Rotation aligns to clock boundaries. 1 hour rotates at 1:00, 2:00, etc.
              </v-alert>

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
                  v-if="settingsStore.deviceSettings.rotationMode === 'sdcard'"
                  variant="tonal"
                  class="mb-4"
                >
                  <v-card-text>
                    <v-select
                      v-model="settingsStore.deviceSettings.sdRotationMode"
                      :items="sdRotationModeOptions"
                      item-title="title"
                      item-value="value"
                      label="SD Card Rotation Logic"
                      variant="outlined"
                      hide-details
                    />
                  </v-card-text>
                </v-card>
              </v-expand-transition>

              <v-expand-transition>
                <v-card
                  v-if="settingsStore.deviceSettings.rotationMode === 'url'"
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

                    <v-checkbox
                      v-if="appStore.systemInfo.sdcard_inserted"
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

              <v-expand-transition>
                <v-card
                  v-if="settingsStore.deviceSettings.rotationMode === 'ai'"
                  variant="tonal"
                  class="mb-4"
                >
                  <v-card-text>
                    <v-select
                      v-model="settingsStore.deviceSettings.aiSettings.aiProvider"
                      :items="aiProviderOptions"
                      item-title="title"
                      item-value="value"
                      label="AI Provider"
                      variant="outlined"
                      class="mb-4"
                      hide-details
                    />
                    <v-select
                      v-model="settingsStore.deviceSettings.aiSettings.aiModel"
                      :items="aiModelOptions"
                      item-title="title"
                      item-value="value"
                      label="AI Model"
                      variant="outlined"
                      class="mb-4"
                      hide-details
                    />
                    <v-textarea
                      v-model="settingsStore.deviceSettings.aiSettings.aiPrompt"
                      label="Default Prompt"
                      variant="outlined"
                      rows="3"
                      hide-details="auto"
                      class="mb-2"
                      hint="Describe the image you want to generate. Use {random} for random seed."
                      persistent-hint
                    />
                    <div
                      v-if="
                        !settingsStore.deviceSettings.aiSettings.openaiApiKey &&
                        !settingsStore.deviceSettings.aiSettings.googleApiKey
                      "
                      class="text-caption text-warning mt-2"
                    >
                      <v-icon icon="mdi-alert" size="small" class="mr-1" />
                      Please configure API keys in the <strong>AI Generation</strong> tab.
                    </div>
                  </v-card-text>
                </v-card>
              </v-expand-transition>
            </div>

            <v-divider class="my-4" />

            <v-switch
              v-model="settingsStore.deviceSettings.sleepScheduleEnabled"
              label="Enable Sleep Schedule"
              color="primary"
              class="mb-2"
              hide-details
            />
            <div class="ml-10">
              <v-row>
                <v-col cols="6" md="3">
                  <v-text-field
                    v-model="settingsStore.deviceSettings.sleepScheduleStart"
                    label="From"
                    type="time"
                    variant="outlined"
                    :disabled="!settingsStore.deviceSettings.sleepScheduleEnabled"
                  />
                </v-col>
                <v-col cols="6" md="3">
                  <v-text-field
                    v-model="settingsStore.deviceSettings.sleepScheduleEnd"
                    label="To"
                    type="time"
                    variant="outlined"
                    :disabled="!settingsStore.deviceSettings.sleepScheduleEnabled"
                  />
                </v-col>
              </v-row>

              <v-alert type="info" variant="tonal" density="compact">
                Images won't rotate during this period. Useful for night hours.
              </v-alert>
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
              <!-- Preset Selection -->
              <v-row>
                <v-col cols="12">
                  <v-card variant="outlined" class="mb-6">
                    <v-card-subtitle class="pt-3"> Processing Preset </v-card-subtitle>
                    <v-card-text>
                      <v-btn-toggle
                        :model-value="settingsStore.preset"
                        mandatory
                        color="primary"
                        variant="outlined"
                        @update:model-value="onPresetChange"
                      >
                        <v-btn v-for="p in presetOptions" :key="p.value" :value="p.value">
                          {{ p.title }}
                        </v-btn>
                      </v-btn-toggle>
                    </v-card-text>
                  </v-card>
                </v-col>

                <!-- Dither Algorithm -->
                <v-col cols="12" md="4">
                  <v-select
                    v-model="settingsStore.params.ditherAlgorithm"
                    :items="ditherOptions"
                    item-title="title"
                    item-value="value"
                    label="Dithering Algorithm"
                    variant="outlined"
                    density="compact"
                  />
                </v-col>

                <v-col cols="12" md="4">
                  <v-select
                    v-model="settingsStore.params.colorMethod"
                    :items="colorMethodOptions"
                    item-title="title"
                    item-value="value"
                    label="Color Matching"
                    variant="outlined"
                    density="compact"
                  />
                </v-col>
              </v-row>

              <v-row>
                <!-- Exposure -->
                <v-col cols="12" md="4">
                  <v-slider
                    v-model="settingsStore.params.exposure"
                    :min="0.5"
                    :max="2.0"
                    :step="0.01"
                    label="Exposure"
                    thumb-label
                    color="primary"
                  >
                    <template #append>
                      <span class="text-body-2">{{
                        settingsStore.params.exposure.toFixed(2)
                      }}</span>
                    </template>
                  </v-slider>
                </v-col>

                <!-- Saturation -->
                <v-col cols="12" md="4">
                  <v-slider
                    v-model="settingsStore.params.saturation"
                    :min="0.5"
                    :max="2.0"
                    :step="0.01"
                    label="Saturation"
                    thumb-label
                    color="primary"
                  >
                    <template #append>
                      <span class="text-body-2">{{
                        settingsStore.params.saturation.toFixed(2)
                      }}</span>
                    </template>
                  </v-slider>
                </v-col>

                <!-- Compress Dynamic Range -->
                <v-col cols="12" md="4">
                  <v-checkbox
                    v-model="settingsStore.params.compressDynamicRange"
                    label="Compress Dynamic Range"
                    hint="Map brightness to display's actual white point"
                    persistent-hint
                    color="primary"
                  />
                </v-col>
              </v-row>

              <!-- Tone Mode -->
              <v-row>
                <v-col cols="12" md="4">
                  <v-select
                    v-model="settingsStore.params.toneMode"
                    :items="toneModeOptions"
                    item-title="title"
                    item-value="value"
                    label="Tone Mapping"
                    variant="outlined"
                    density="compact"
                  />
                </v-col>

                <!-- Contrast (for contrast mode) -->
                <v-col v-if="!isScurveMode" cols="12" md="4">
                  <v-slider
                    v-model="settingsStore.params.contrast"
                    :min="0.5"
                    :max="2.0"
                    :step="0.01"
                    label="Contrast"
                    thumb-label
                    color="primary"
                  >
                    <template #append>
                      <span class="text-body-2">{{
                        settingsStore.params.contrast.toFixed(2)
                      }}</span>
                    </template>
                  </v-slider>
                </v-col>
              </v-row>

              <!-- S-Curve Controls -->
              <v-expand-transition>
                <v-card v-if="isScurveMode" variant="tonal" class="mt-4">
                  <v-card-subtitle class="pt-3"> S-Curve Parameters </v-card-subtitle>
                  <v-card-text>
                    <v-row>
                      <v-col cols="12" md="6">
                        <v-slider
                          v-model="settingsStore.params.strength"
                          :min="0"
                          :max="1"
                          :step="0.01"
                          label="Strength"
                          thumb-label
                          color="primary"
                        >
                          <template #append>
                            <span class="text-body-2">{{
                              settingsStore.params.strength.toFixed(2)
                            }}</span>
                          </template>
                        </v-slider>
                      </v-col>

                      <v-col cols="12" md="6">
                        <v-slider
                          v-model="settingsStore.params.shadowBoost"
                          :min="0"
                          :max="1"
                          :step="0.01"
                          label="Shadow Boost"
                          thumb-label
                          color="primary"
                        >
                          <template #append>
                            <span class="text-body-2">{{
                              settingsStore.params.shadowBoost.toFixed(2)
                            }}</span>
                          </template>
                        </v-slider>
                      </v-col>

                      <v-col cols="12" md="6">
                        <v-slider
                          v-model="settingsStore.params.highlightCompress"
                          :min="0.5"
                          :max="5"
                          :step="0.01"
                          label="Highlight Compress"
                          thumb-label
                          color="primary"
                        >
                          <template #append>
                            <span class="text-body-2">{{
                              settingsStore.params.highlightCompress.toFixed(2)
                            }}</span>
                          </template>
                        </v-slider>
                      </v-col>

                      <v-col cols="12" md="6">
                        <v-slider
                          v-model="settingsStore.params.midpoint"
                          :min="0.3"
                          :max="0.7"
                          :step="0.01"
                          label="Midpoint"
                          thumb-label
                          color="primary"
                        >
                          <template #append>
                            <span class="text-body-2">{{
                              settingsStore.params.midpoint.toFixed(2)
                            }}</span>
                          </template>
                        </v-slider>
                      </v-col>
                    </v-row>
                  </v-card-text>
                </v-card>
              </v-expand-transition>
            </div>
          </v-tabs-window-item>

          <!-- AI Generation Tab -->
          <v-tabs-window-item value="ai">
            <v-select
              v-model="settingsStore.deviceSettings.aiSettings.aiProvider"
              :items="aiProviderOptions"
              item-title="title"
              item-value="value"
              label="AI Provider"
              variant="outlined"
              class="mb-4"
            />
            <v-select
              v-model="settingsStore.deviceSettings.aiSettings.aiModel"
              :items="aiModelOptions"
              item-title="title"
              item-value="value"
              label="AI Model"
              variant="outlined"
              class="mb-4"
            />

            <v-expand-transition>
              <div v-if="settingsStore.deviceSettings.aiSettings.aiProvider === 0">
                <v-text-field
                  v-model="settingsStore.deviceSettings.aiSettings.openaiApiKey"
                  label="OpenAI API Key"
                  variant="outlined"
                  type="password"
                  hint="sk-..."
                  persistent-hint
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
              </div>
            </v-expand-transition>

            <v-expand-transition>
              <div v-if="settingsStore.deviceSettings.aiSettings.aiProvider === 1">
                <v-text-field
                  v-model="settingsStore.deviceSettings.aiSettings.googleApiKey"
                  label="Google Gemini API Key"
                  variant="outlined"
                  type="password"
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
              </div>
            </v-expand-transition>
          </v-tabs-window-item>

          <!-- Calibration Tab -->
          <v-tabs-window-item value="calibration">
            <PaletteCalibration />
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
        <v-btn color="primary" :loading="saving" @click="saveSettings">
          <v-icon icon="mdi-content-save" start />
          Save Settings
        </v-btn>
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
  </div>
</template>

<style scoped></style>
