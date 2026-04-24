<script setup>
import { ref, onMounted, watch } from "vue";
import { getPreset, getPresetOptions, getDefaultParams } from "@aitjcize/epaper-image-convert";
import { useSettingsStore } from "../stores";
import ImageProcessing from "../components/ImageProcessing.vue";
import ProcessingControls from "../components/ProcessingControls.vue";
import boardsData from "../../../boards/boards.json";

const settingsStore = useSettingsStore();
// Default demo to portrait since the sample image is portrait
settingsStore.deviceSettings.displayOrientation = "portrait";

// State
const tab = ref("demo");
const stableVersion = ref("-");
const devVersion = ref("Loading...");
const selectedVersion = ref("stable");
const selectedBoard = ref("waveshare_photopainter_73");
const baseUrl = import.meta.env.BASE_URL;

const appFeatures = [
  { icon: "mdi-access-point-network", text: "Auto Discovery" },
  { icon: "mdi-image-multiple", text: "Gallery Management" },
  { icon: "mdi-auto-fix", text: "AI Image Generation" },
  { icon: "mdi-wifi", text: "WiFi Provisioning" },
  { icon: "mdi-cog", text: "Device Settings" },
  { icon: "mdi-update", text: "OTA Updates" },
  { icon: "mdi-camera", text: "Camera Upload" },
  { icon: "mdi-palette", text: "Image Processing" },
];

const supportedBoards = boardsData.map((b) => ({
  ...b,
  value: b.id,
  resolution: `${b.resolution[0]} × ${b.resolution[1]}`,
}));

// Image state
const fileInput = ref(null);
const selectedFile = ref(null);

// Processing parameters
const currentPreset = ref("balanced");
const params = ref(getDefaultParams());

// Get preset names for matching
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

watch(
  params,
  () => {
    currentPreset.value = derivePresetFromParams();
  },
  { deep: true }
);

onMounted(async () => {
  // Handle URL hash for tab selection
  const hash = window.location.hash.slice(1);
  if (hash === "demo" || hash === "flash" || hash === "app") {
    tab.value = hash;
  }

  loadVersionInfo();
  loadSampleImage();
});

// Watch tab or board changes
watch([tab, selectedBoard], ([newTab]) => {
  if (newTab) window.location.hash = newTab;
  loadVersionInfo();
});

async function loadVersionInfo() {
  try {
    // Fetch stable version
    const stableResponse = await fetch(
      "https://api.github.com/repos/aitjcize/esp32-photoframe/releases/latest"
    );
    const stableData = await stableResponse.json();
    stableVersion.value = stableData.tag_name;

    // Get dev version from manifest-dev.json (try board-specific first)
    let devResponse = await fetch(baseUrl + selectedBoard.value + "/manifest-dev.json");
    if (!devResponse.ok) {
      devResponse = await fetch(baseUrl + "manifest-dev.json");
    }
    if (devResponse.ok) {
      const devData = await devResponse.json();
      devVersion.value = devData.version || "dev";
    }
  } catch (error) {
    console.error("Error fetching version info:", error);
    stableVersion.value = "Unknown";
  }
}

async function loadSampleImage() {
  try {
    const response = await fetch(baseUrl + "sample.jpg");
    if (!response.ok) {
      console.log("Sample image not found, waiting for user upload");
      return;
    }
    const blob = await response.blob();
    selectedFile.value = new File([blob], "sample.jpg", { type: "image/jpeg" });
  } catch (error) {
    console.log("Sample image not available:", error.message);
  }
}

function triggerFileSelect() {
  fileInput.value?.click();
}

async function onFileSelected(event) {
  const file = event.target.files?.[0];
  if (file) {
    selectedFile.value = file;
  }
}

async function onDrop(event) {
  const file = event.dataTransfer?.files?.[0];
  if (file && file.type.match("image.*")) {
    selectedFile.value = file;
  }
}

function onPresetChange(presetName) {
  if (presetName === "custom") return;

  const preset = getPreset(presetName);
  if (preset) {
    // eslint-disable-next-line no-unused-vars
    const { name, title, description, ...processingParams } = preset;
    Object.assign(params.value, processingParams);
    currentPreset.value = presetName;
  }
}

function onParamsUpdate(newParams) {
  Object.assign(params.value, newParams);
}

function newImage() {
  selectedFile.value = null;
  if (fileInput.value) {
    fileInput.value.value = "";
  }
}
</script>

<template>
  <v-app>
    <!-- Header -->
    <v-app-bar color="primary" dark>
      <template #prepend>
        <v-img :src="`${baseUrl}icon.svg`" alt="PhotoFrame" width="40" height="40" class="ml-2" />
      </template>
      <v-toolbar-title class="ml-4">ESP32 PhotoFrame - Demo & Flash</v-toolbar-title>
      <v-spacer />
      <v-chip variant="outlined" class="mr-2">
        {{ stableVersion }}
      </v-chip>
      <v-btn
        icon
        variant="text"
        href="https://github.com/aitjcize/esp32-photoframe"
        target="_blank"
        title="View on GitHub"
      >
        <v-icon icon="mdi-github" />
      </v-btn>
    </v-app-bar>

    <v-main class="bg-grey-lighten-4">
      <v-container class="py-6" style="max-width: 1400px">
        <!-- Hidden file input -->
        <input
          ref="fileInput"
          type="file"
          accept="image/*"
          style="display: none"
          @change="onFileSelected"
        />

        <!-- Project Intro -->
        <v-card class="mb-4">
          <v-card-text class="pa-6">
            <div class="d-flex align-center mb-4">
              <v-img
                :src="`${baseUrl}icon.svg`"
                alt="ESP32 PhotoFrame"
                width="64"
                height="64"
                class="mr-4 flex-grow-0"
              />
              <div>
                <div class="text-h5 font-weight-bold">ESP32 PhotoFrame</div>
                <div class="text-body-2 text-grey-darken-1">
                  Open-source firmware for ESP32-based e-paper photo frames
                </div>
              </div>
            </div>
            <p class="text-body-1 mb-4">
              A drop-in replacement for stock e-paper photo-frame firmware, with significantly
              better image quality, a REST API and web interface, client-side AI image generation,
              and Home Assistant integration. Designed for always-on ambient display with weeks of
              battery life on deep sleep.
            </p>
            <v-row>
              <v-col cols="6" md="3">
                <v-card variant="tonal" class="text-center pa-4" height="100%">
                  <v-icon icon="mdi-palette" size="32" color="primary" class="mb-2" />
                  <div class="text-subtitle-2 font-weight-bold">Measured Palette</div>
                  <div class="text-caption">
                    Dither against real panel colors for accurate, non-washed-out output
                  </div>
                </v-card>
              </v-col>
              <v-col cols="6" md="3">
                <v-card variant="tonal" class="text-center pa-4" height="100%">
                  <v-icon icon="mdi-zip-box" size="32" color="primary" class="mb-2" />
                  <div class="text-subtitle-2 font-weight-bold">EPDGZ Format</div>
                  <div class="text-caption">
                    4 bpp pre-processed format for small files and instant render
                  </div>
                </v-card>
              </v-col>
              <v-col cols="6" md="3">
                <v-card variant="tonal" class="text-center pa-4" height="100%">
                  <v-icon icon="mdi-battery-charging" size="32" color="primary" class="mb-2" />
                  <div class="text-subtitle-2 font-weight-bold">Deep Sleep</div>
                  <div class="text-caption">
                    Weeks of battery life, or always-on for Home Assistant
                  </div>
                </v-card>
              </v-col>
              <v-col cols="6" md="3">
                <v-card variant="tonal" class="text-center pa-4" height="100%">
                  <v-icon icon="mdi-api" size="32" color="primary" class="mb-2" />
                  <div class="text-subtitle-2 font-weight-bold">REST API + Web UI</div>
                  <div class="text-caption">
                    Full programmatic control, drag-and-drop uploads, live status
                  </div>
                </v-card>
              </v-col>
            </v-row>
            <div class="mt-4 d-flex flex-wrap ga-2">
              <v-btn
                variant="tonal"
                size="small"
                href="https://github.com/aitjcize/esp32-photoframe"
                target="_blank"
                prepend-icon="mdi-github"
              >
                Firmware
              </v-btn>
              <v-btn
                variant="tonal"
                size="small"
                href="https://github.com/aitjcize/esp32-photoframe-server"
                target="_blank"
                prepend-icon="mdi-server"
              >
                Image Server
              </v-btn>
              <v-btn
                variant="tonal"
                size="small"
                href="https://github.com/aitjcize/ha-esp32-photoframe"
                target="_blank"
                prepend-icon="mdi-home-assistant"
              >
                Home Assistant
              </v-btn>
              <v-btn
                variant="tonal"
                size="small"
                href="https://github.com/aitjcize/esp32-photoframe-app"
                target="_blank"
                prepend-icon="mdi-cellphone"
              >
                Mobile App
                <v-chip size="x-small" class="ml-2" color="info" variant="flat">
                  Testing
                </v-chip>
              </v-btn>
            </div>
          </v-card-text>
        </v-card>

        <!-- Supported Hardware -->
        <v-card class="mb-6">
          <v-card-title>
            <v-icon icon="mdi-chip" class="mr-2" />
            Supported Hardware
          </v-card-title>
          <v-card-text>
            <v-table density="comfortable">
              <thead>
                <tr>
                  <th>Board</th>
                  <th>Display</th>
                  <th>Resolution</th>
                  <th>Storage</th>
                </tr>
              </thead>
              <tbody>
                <tr v-for="b in supportedBoards" :key="b.value">
                  <td class="font-weight-medium">
                    <a :href="b.url" target="_blank" rel="noopener" class="board-link">
                      {{ b.label }}
                      <v-icon icon="mdi-open-in-new" size="x-small" class="ml-1" />
                    </a>
                  </td>
                  <td>{{ b.display }}</td>
                  <td>{{ b.resolution }}</td>
                  <td>{{ b.storage }}</td>
                </tr>
              </tbody>
            </v-table>
          </v-card-text>
        </v-card>

        <!-- Tabs -->
        <v-tabs v-model="tab" color="primary" class="mb-6">
          <v-tab value="demo">
            <v-icon icon="mdi-image-edit" start />
            Image Processing Demo
          </v-tab>
          <v-tab value="flash">
            <v-icon icon="mdi-flash" start />
            Flash Firmware
          </v-tab>
          <v-tab value="app">
            <v-icon icon="mdi-cellphone-arrow-down" start />
            Companion App
          </v-tab>
        </v-tabs>

        <v-tabs-window v-model="tab">
          <!-- Demo Tab -->
          <v-tabs-window-item value="demo">
            <!-- Info Banner -->
            <v-alert type="info" variant="tonal" class="mb-4">
              <template #title>
                <v-icon icon="mdi-auto-fix" class="mr-2" />
                Interactive Comparison
              </template>
              Drag the slider to compare our enhanced algorithm with the original image. Our
              algorithm features S-curve tone mapping, saturation adjustment, and measured palette
              dithering for superior e-paper display quality.
            </v-alert>

            <!-- Preview Card -->
            <v-card class="mb-4">
              <v-card-title>
                <v-icon icon="mdi-compare" class="mr-2" />
                Original vs Processed Comparison
                <v-spacer />
                <v-btn v-if="selectedFile" variant="text" size="small" @click="newImage">
                  <v-icon icon="mdi-refresh" start />
                  New Image
                </v-btn>
              </v-card-title>

              <v-card-text>
                <!-- Upload Area -->
                <v-sheet
                  v-if="!selectedFile"
                  class="d-flex flex-column align-center justify-center pa-8"
                  color="grey-lighten-3"
                  rounded
                  min-height="300"
                  style="cursor: pointer; border: 2px dashed #ccc"
                  @click="triggerFileSelect"
                  @dragover.prevent
                  @drop.prevent="onDrop"
                >
                  <v-icon icon="mdi-cloud-upload" size="64" color="grey" />
                  <p class="text-h6 mt-4">Click or drag image to upload</p>
                  <p class="text-body-2 text-grey">Supports: JPG, PNG, WebP</p>
                </v-sheet>

                <!-- Image Processing Component -->
                <ImageProcessing v-else :image-file="selectedFile" :params="params" />
              </v-card-text>
            </v-card>

            <!-- Processing Controls Card -->
            <v-card>
              <v-card-title>
                <v-icon icon="mdi-tune" class="mr-2" />
                Processing Parameters
              </v-card-title>
              <v-card-text>
                <ProcessingControls
                  :params="params"
                  :preset="currentPreset"
                  @update:params="onParamsUpdate"
                  @preset-change="onPresetChange"
                />
              </v-card-text>
            </v-card>
          </v-tabs-window-item>

          <!-- Flash Tab -->
          <v-tabs-window-item value="flash">
            <v-row justify="center">
              <v-col cols="12" md="10">
                <!-- Requirements -->
                <v-alert type="warning" variant="tonal" class="mb-4">
                  <template #title>
                    <v-icon icon="mdi-alert" class="mr-2" />
                    Requirements
                  </template>
                  <ul class="pl-4 mt-2">
                    <li>Chrome, Edge, or Opera browser (Web Serial API required)</li>
                    <li>USB-C cable connected to your ESP32-S3 PhotoFrame</li>
                    <li>Compatible ESP32-S3 board (Waveshare, Seeed XIAO, or Seeed reTerminal)</li>
                  </ul>
                </v-alert>

                <!-- Flash Card -->
                <v-card class="mb-4">
                  <v-card-title>
                    <v-icon icon="mdi-flash" class="mr-2" />
                    Flash Firmware
                  </v-card-title>
                  <v-card-text>
                    <div class="text-subtitle-1 mb-2">Select Version:</div>
                    <v-radio-group v-model="selectedVersion" class="mb-4">
                      <v-radio value="stable">
                        <template #label>
                          <span>Stable Release</span>
                          <v-chip size="small" color="success" class="ml-2">{{
                            stableVersion
                          }}</v-chip>
                        </template>
                      </v-radio>
                      <v-radio value="dev">
                        <template #label>
                          <span>Development Build</span>
                          <v-chip size="small" color="warning" class="ml-2">{{
                            devVersion
                          }}</v-chip>
                        </template>
                      </v-radio>
                    </v-radio-group>

                    <v-divider class="mb-4" />

                    <v-select
                      v-model="selectedBoard"
                      :items="supportedBoards"
                      item-title="label"
                      item-value="value"
                      label="Select Board"
                      class="mb-4 mt-4"
                    />

                    <div class="d-flex justify-center">
                      <esp-web-install-button
                        :key="selectedBoard + selectedVersion"
                        :manifest="
                          (baseUrl.endsWith('/') ? baseUrl : baseUrl + '/') +
                          selectedBoard +
                          '/' +
                          (selectedVersion === 'stable' ? 'manifest.json' : 'manifest-dev.json')
                        "
                      >
                        <template #activate>
                          <button class="flash-button">
                            <v-icon icon="mdi-flash" class="mr-2" />
                            Install Firmware
                          </button>
                        </template>
                      </esp-web-install-button>
                    </div>
                  </v-card-text>
                </v-card>

                <!-- Instructions -->
                <v-card>
                  <v-card-title>
                    <v-icon icon="mdi-format-list-numbered" class="mr-2" />
                    Instructions
                  </v-card-title>
                  <v-card-text>
                    <ol class="pl-4">
                      <li class="mb-2">Connect your ESP32-S3 board to your computer via USB</li>
                      <li class="mb-2">
                        Click "Install Firmware" and select the correct serial port
                      </li>
                      <li class="mb-2">Wait for the flashing process to complete</li>
                      <li class="mb-2">
                        The device will restart and create a WiFi access point named
                        "PhotoFrame-XXXX"
                      </li>
                      <li>Connect to the access point and configure your WiFi settings</li>
                    </ol>
                  </v-card-text>
                </v-card>
              </v-col>
            </v-row>
          </v-tabs-window-item>

          <!-- App Tab -->
          <v-tabs-window-item value="app">
            <v-row justify="center">
              <v-col cols="12" md="8">
                <v-card class="text-center pa-8">
                  <v-icon icon="mdi-cellphone-arrow-down" size="80" color="primary" class="mb-4" />
                  <div class="text-h4 font-weight-bold mb-2">ESP Frame Companion App</div>
                  <div class="text-body-1 text-grey-darken-1 mb-6">
                    Manage your e-paper photo frame from your phone. Discover devices, upload
                    photos, generate AI images, and configure settings — all from one app.
                  </div>

                  <v-chip color="info" size="large" class="mb-4">
                    <v-icon icon="mdi-flask-outline" start />
                    Available for Public Testing
                  </v-chip>

                  <v-alert
                    type="info"
                    variant="tonal"
                    density="comfortable"
                    class="text-left mb-8"
                  >
                    The app is in public beta via TestFlight (iOS) and Google Play internal
                    testing (Android). Tap a store below for install instructions, or open the
                    <a
                      href="https://github.com/aitjcize/esp32-photoframe/discussions/86"
                      target="_blank"
                      rel="noopener"
                      >testing discussion</a
                    >
                    to report feedback and bugs.
                  </v-alert>

                  <v-row justify="center" class="mb-6">
                    <v-col cols="auto">
                      <v-card
                        variant="outlined"
                        class="pa-4"
                        style="min-width: 180px; cursor: pointer"
                        href="https://github.com/aitjcize/esp32-photoframe/discussions/86"
                        target="_blank"
                        rel="noopener"
                      >
                        <v-icon icon="mdi-apple" size="40" class="mb-2" />
                        <div class="text-subtitle-2">App Store</div>
                        <div class="text-caption text-grey-darken-1">TestFlight</div>
                      </v-card>
                    </v-col>
                    <v-col cols="auto">
                      <v-card
                        variant="outlined"
                        class="pa-4"
                        style="min-width: 180px; cursor: pointer"
                        href="https://github.com/aitjcize/esp32-photoframe/discussions/86"
                        target="_blank"
                        rel="noopener"
                      >
                        <v-icon icon="mdi-google-play" size="40" class="mb-2" />
                        <div class="text-subtitle-2">Google Play</div>
                        <div class="text-caption text-grey-darken-1">Internal Testing</div>
                      </v-card>
                    </v-col>
                  </v-row>

                  <v-divider class="mb-6" />

                  <div class="text-h6 mb-4">Features</div>
                  <v-row justify="center">
                    <v-col
                      cols="6"
                      sm="4"
                      md="3"
                      v-for="feature in appFeatures"
                      :key="feature.icon"
                    >
                      <v-icon :icon="feature.icon" size="32" color="primary" class="mb-2" />
                      <div class="text-caption">{{ feature.text }}</div>
                    </v-col>
                  </v-row>
                </v-card>
              </v-col>
            </v-row>
          </v-tabs-window-item>
        </v-tabs-window>
      </v-container>
    </v-main>
  </v-app>
</template>

<style scoped>
.board-link {
  color: rgb(var(--v-theme-primary));
  text-decoration: none;
}
.board-link:hover {
  text-decoration: underline;
}

.flash-button {
  padding: 16px 32px;
  font-size: 1.1rem;
  background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
  color: white;
  border: none;
  border-radius: 8px;
  cursor: pointer;
  display: flex;
  align-items: center;
  transition: all 0.3s;
}

.flash-button:hover {
  transform: translateY(-2px);
  box-shadow: 0 5px 15px rgba(102, 126, 234, 0.4);
}
</style>
