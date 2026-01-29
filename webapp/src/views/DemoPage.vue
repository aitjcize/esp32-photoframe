<script setup>
import { ref, onMounted, watch } from "vue";
import { getPreset, getPresetOptions } from "@aitjcize/epaper-image-convert";
import ImageProcessing from "../components/ImageProcessing.vue";
import ProcessingControls from "../components/ProcessingControls.vue";
import { useAppStore } from "../stores/app";

// State
const tab = ref("demo");
const appStore = useAppStore();
const stableVersion = ref("-");
const devVersion = ref("Loading...");
const selectedVersion = ref("stable");
const selectedBoard = ref("waveshare_photopainter_73");
const baseUrl = import.meta.env.BASE_URL;

const boardOptions = [
  { label: 'Waveshare 7.3" (7-color)', value: "waveshare_photopainter_73" },
  { label: "Seeed Studio XIAO EE02", value: "seeedstudio_xiao_ee02" },
];

// Image state
const fileInput = ref(null);
const selectedFile = ref(null);

// Processing parameters
const currentPreset = ref("balanced");
const params = ref({
  exposure: 1.0,
  saturation: 1.3,
  toneMode: "scurve",
  contrast: 1.0,
  strength: 0.9,
  shadowBoost: 0.0,
  highlightCompress: 1.5,
  midpoint: 0.5,
  colorMethod: "rgb",
  ditherAlgorithm: "floyd-steinberg",
  compressDynamicRange: true,
});

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
  if (hash === "demo" || hash === "flash") {
    tab.value = hash;
  }

  loadVersionInfo();
  loadSampleImage();
});

// Watch tab changes and update URL hash
watch(tab, (newTab) => {
  window.location.hash = newTab;
});

async function loadVersionInfo() {
  try {
    // Fetch stable version
    const stableResponse = await fetch(
      "https://api.github.com/repos/aitjcize/esp32-photoframe/releases/latest"
    );
    const stableData = await stableResponse.json();
    stableVersion.value = stableData.tag_name;

    // Get dev version from manifest-dev.json (try root first, then board-specific)
    let devResponse = await fetch(baseUrl + "manifest-dev.json");
    if (!devResponse.ok) {
      devResponse = await fetch(baseUrl + selectedBoard.value + "/manifest-dev.json");
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
      <v-toolbar-title>
        <v-icon icon="mdi-image-frame" class="mr-2" />
        ESP32 PhotoFrame - Demo & Flash
      </v-toolbar-title>
      <v-spacer />
      <v-chip variant="outlined" class="mr-2">
        {{ stableVersion }}
      </v-chip>
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
                <!-- About Section -->
                <v-card class="mb-4">
                  <v-card-title>
                    <v-icon icon="mdi-information" class="mr-2" />
                    About
                  </v-card-title>
                  <v-card-text>
                    <p class="mb-4">
                      A modern firmware for the Waveshare ESP32-S3-PhotoPainter that provides a
                      powerful RESTful API and web interface for managing your e-paper photo frame.
                    </p>

                    <!-- Feature Grid -->
                    <v-row>
                      <v-col cols="6" md="3">
                        <v-card variant="tonal" class="text-center pa-4">
                          <v-icon icon="mdi-camera" size="32" color="primary" class="mb-2" />
                          <div class="text-subtitle-2 font-weight-bold">Smart Upload</div>
                          <div class="text-caption">
                            Drag & drop JPEG files with automatic processing
                          </div>
                        </v-card>
                      </v-col>
                      <v-col cols="6" md="3">
                        <v-card variant="tonal" class="text-center pa-4">
                          <v-icon icon="mdi-rotate-right" size="32" color="primary" class="mb-2" />
                          <div class="text-subtitle-2 font-weight-bold">Auto-Rotate</div>
                          <div class="text-caption">
                            Automatic image rotation with configurable intervals
                          </div>
                        </v-card>
                      </v-col>
                      <v-col cols="6" md="3">
                        <v-card variant="tonal" class="text-center pa-4">
                          <v-icon
                            icon="mdi-battery-charging"
                            size="32"
                            color="primary"
                            class="mb-2"
                          />
                          <div class="text-subtitle-2 font-weight-bold">Power Smart</div>
                          <div class="text-caption">
                            2-minute auto-sleep with battery monitoring
                          </div>
                        </v-card>
                      </v-col>
                      <v-col cols="6" md="3">
                        <v-card variant="tonal" class="text-center pa-4">
                          <v-icon
                            icon="mdi-cellphone-link"
                            size="32"
                            color="primary"
                            class="mb-2"
                          />
                          <div class="text-subtitle-2 font-weight-bold">Web Control</div>
                          <div class="text-caption">
                            Modern web interface with real-time updates
                          </div>
                        </v-card>
                      </v-col>
                    </v-row>
                  </v-card-text>
                </v-card>

                <!-- Requirements -->
                <v-alert type="warning" variant="tonal" class="mb-4">
                  <template #title>
                    <v-icon icon="mdi-alert" class="mr-2" />
                    Requirements
                  </template>
                  <ul class="pl-4 mt-2">
                    <li>Chrome, Edge, or Opera browser (Web Serial API required)</li>
                    <li>USB-C cable connected to your ESP32-S3 PhotoFrame</li>
                    <li>Compatible ESP32-S3 board (Waveshare or Seeed XIAO)</li>
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

                    <div class="text-subtitle-1 mb-2 mt-4">Select Board:</div>
                    <v-radio-group v-model="selectedBoard" class="mb-4">
                      <v-radio
                        v-for="board in boardOptions"
                        :key="board.value"
                        :label="board.label"
                        :value="board.value"
                      />
                    </v-radio-group>

                    <div class="d-flex justify-center">
                      <esp-web-install-button
                        :manifest="
                          baseUrl +
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
        </v-tabs-window>
      </v-container>
    </v-main>
  </v-app>
</template>

<style scoped>
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
