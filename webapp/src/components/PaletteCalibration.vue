<script setup>
import { ref, watch } from "vue";
import { useSettingsStore } from "../stores";
import { getCanvasContext, rgbToLab } from "@aitjcize/epaper-image-convert";

// LAB to RGB conversion (not exported from library)
function labToRgb(L, a, b) {
  // LAB to XYZ
  let y = (L + 16) / 116;
  let x = a / 500 + y;
  let z = y - b / 200;

  const delta = 6 / 29;
  const f = (t) => (t > delta ? t * t * t : 3 * delta * delta * (t - 4 / 29));

  // D65 illuminant
  x = 0.95047 * f(x);
  y = 1.0 * f(y);
  z = 1.08883 * f(z);

  // XYZ to RGB
  let r = x * 3.2406 + y * -1.5372 + z * -0.4986;
  let g = x * -0.9689 + y * 1.8758 + z * 0.0415;
  let bOut = x * 0.0557 + y * -0.204 + z * 1.057;

  // Gamma correction
  const gamma = (c) => (c > 0.0031308 ? 1.055 * Math.pow(c, 1 / 2.4) - 0.055 : 12.92 * c);
  r = Math.round(Math.max(0, Math.min(255, gamma(r) * 255)));
  g = Math.round(Math.max(0, Math.min(255, gamma(g) * 255)));
  bOut = Math.round(Math.max(0, Math.min(255, gamma(bOut) * 255)));

  return [r, g, bOut];
}

const settingsStore = useSettingsStore();

const API_BASE = "";

// Calibration flow state
const calibrationStep = ref(0); // 0 = not started, 1-4 = steps
const loading = ref(false);
const statusMessage = ref("");
const statusType = ref("info");
const measuredPalette = ref(null);
const calibrationCanvas = ref(null);
const hasUploadedPhoto = ref(false);
const showResetDialog = ref(false);

const colorNames = ["black", "white", "yellow", "red", "blue", "green"];

// Store original palette to restore on cancel
let originalPalette = null;

// Watch measuredPalette changes during review step to update preview in real-time
watch(
  measuredPalette,
  (newPalette) => {
    if (newPalette && calibrationStep.value === 4) {
      // Update settingsStore.palette to trigger preview update
      Object.assign(settingsStore.palette, newPalette);
    }
  },
  { deep: true }
);

function startCalibration() {
  // Save original palette to restore on cancel
  originalPalette = JSON.parse(JSON.stringify(settingsStore.palette));
  calibrationStep.value = 1;
  statusMessage.value = "";
  measuredPalette.value = null;
  hasUploadedPhoto.value = false;
}

function cancelCalibration() {
  // Restore original palette if we modified it during review
  if (originalPalette) {
    Object.assign(settingsStore.palette, originalPalette);
    originalPalette = null;
  }
  calibrationStep.value = 0;
  statusMessage.value = "";
  measuredPalette.value = null;
}

async function displayCalibrationPattern() {
  loading.value = true;
  statusMessage.value = "Displaying calibration pattern on device...";
  statusType.value = "info";

  try {
    const response = await fetch(`${API_BASE}/api/calibration/display`, {
      method: "POST",
    });

    if (response.ok) {
      statusMessage.value = "Calibration pattern displayed on device";
      statusType.value = "success";
      setTimeout(() => {
        calibrationStep.value = 2;
        statusMessage.value = "";
      }, 1500);
    } else {
      const data = await response.json().catch(() => ({}));
      throw new Error(data.message || "Failed to display calibration pattern");
    }
  } catch (error) {
    if (error.message.includes("Failed to fetch") || error.message.includes("NetworkError")) {
      statusMessage.value =
        "Device not connected. Use 'Skip' if pattern is already displayed, or connect to your device.";
    } else {
      statusMessage.value = `Error: ${error.message}`;
    }
    statusType.value = "error";
  } finally {
    loading.value = false;
  }
}

function skipToPhotoStep() {
  calibrationStep.value = 2;
  statusMessage.value = "";
}

function proceedToUpload() {
  calibrationStep.value = 3;
  statusMessage.value = "";
}

async function onPhotoSelected(event) {
  const file = event.target.files[0];
  if (!file) return;

  loading.value = true;
  statusMessage.value = "Analyzing photo...";
  statusType.value = "info";

  try {
    const img = await loadImage(file);
    const canvas = calibrationCanvas.value;
    canvas.width = img.width;
    canvas.height = img.height;
    const ctx = getCanvasContext(canvas);
    ctx.drawImage(img, 0, 0);
    hasUploadedPhoto.value = true;

    // Extract color boxes
    const palette = extractColorBoxes(ctx, img.width, img.height);

    if (palette) {
      const validation = validateExtractedColors(palette);
      if (!validation.valid) {
        throw new Error(validation.message);
      }

      measuredPalette.value = palette;
      statusMessage.value = "Colors extracted successfully";
      statusType.value = "success";

      setTimeout(() => {
        calibrationStep.value = 4;
        statusMessage.value = "";
      }, 1000);
    } else {
      throw new Error(
        "Could not detect color boxes. Please ensure the photo shows the entire display clearly."
      );
    }
  } catch (error) {
    statusMessage.value = `Error: ${error.message}`;
    statusType.value = "error";
  } finally {
    loading.value = false;
  }
}

function loadImage(file) {
  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onload = (e) => {
      const img = new Image();
      img.onload = () => resolve(img);
      img.onerror = reject;
      img.src = e.target.result;
    };
    reader.onerror = reject;
    reader.readAsDataURL(file);
  });
}

function validateExtractedColors(palette) {
  const expectations = {
    black: { maxBrightness: 70, name: "Black" },
    white: { minBrightness: 150, name: "White" },
    yellow: { minR: 150, minG: 150, maxB: 100, name: "Yellow" },
    red: { minR: 100, maxG: 80, maxB: 80, name: "Red" },
    blue: { maxR: 100, maxG: 100, minB: 100, name: "Blue" },
    green: { maxR: 100, minG: 80, maxB: 100, name: "Green" },
  };

  const errors = [];
  const brightness = (color) => (color.r + color.g + color.b) / 3;

  if (brightness(palette.black) > expectations.black.maxBrightness) {
    errors.push(
      `Black is too bright (${Math.round(brightness(palette.black))}). Expected < ${expectations.black.maxBrightness}.`
    );
  }

  if (brightness(palette.white) < expectations.white.minBrightness) {
    errors.push(
      `White is too dark (${Math.round(brightness(palette.white))}). Expected > ${expectations.white.minBrightness}.`
    );
  }

  if (palette.yellow.r < expectations.yellow.minR || palette.yellow.g < expectations.yellow.minG) {
    errors.push(`Yellow doesn't have enough red/green. Check lighting.`);
  }

  if (palette.red.r < expectations.red.minR) {
    errors.push(`Red is not red enough. Check lighting conditions.`);
  }

  if (palette.blue.b < expectations.blue.minB) {
    errors.push(`Blue is not blue enough. Check lighting conditions.`);
  }

  if (palette.green.g < expectations.green.minG) {
    errors.push(`Green is not green enough. Check lighting conditions.`);
  }

  if (errors.length > 0) {
    return {
      valid: false,
      message:
        "Color validation failed:\n• " +
        errors.join("\n• ") +
        "\n\nPlease retake the photo under proper lighting (5500K daylight, no shadows/glare).",
    };
  }

  return { valid: true };
}

function extractColorBoxes(ctx, width, height) {
  const imageData = ctx.getImageData(0, 0, width, height);
  const data = imageData.data;

  // Detect orientation: portrait (height > width) or landscape
  const isPortrait = height > width;
  const cols = isPortrait ? 2 : 3;
  const rows = isPortrait ? 3 : 2;

  // Step 1: Divide image into 6 grid cells and run clustering on each cell
  const cellWidth = width / cols;
  const cellHeight = height / rows;
  const boxes = [];

  for (let row = 0; row < rows; row++) {
    for (let col = 0; col < cols; col++) {
      const cellX = col * cellWidth;
      const cellY = row * cellHeight;

      // Sample pixels from center 90% of the cell (small margin to avoid grid lines)
      const marginX = cellWidth * 0.05;
      const marginY = cellHeight * 0.05;
      const samples = [];
      const step = 2;

      for (let y = cellY + marginY; y < cellY + cellHeight - marginY; y += step) {
        for (let x = cellX + marginX; x < cellX + cellWidth - marginX; x += step) {
          const px = Math.floor(x);
          const py = Math.floor(y);
          if (px >= 0 && px < width && py >= 0 && py < height) {
            const idx = (py * width + px) * 4;
            samples.push({
              x: px,
              y: py,
              r: data[idx],
              g: data[idx + 1],
              b: data[idx + 2],
            });
          }
        }
      }

      // Cluster colors within this cell
      const clusters = clusterColors(samples);

      // Get the dominant cluster (largest by point count)
      clusters.sort((a, b) => b.points.length - a.points.length);
      const dominant = clusters[0];

      // Recalculate median from the dominant cluster's points
      const rValues = dominant.points.map((p) => p.r).sort((a, b) => a - b);
      const gValues = dominant.points.map((p) => p.g).sort((a, b) => a - b);
      const bValues = dominant.points.map((p) => p.b).sort((a, b) => a - b);
      const mid = Math.floor(dominant.points.length / 2);

      // Calculate bounding box from dominant cluster points
      const minX = Math.min(...dominant.points.map((p) => p.x));
      const maxX = Math.max(...dominant.points.map((p) => p.x));
      const minY = Math.min(...dominant.points.map((p) => p.y));
      const maxY = Math.max(...dominant.points.map((p) => p.y));

      boxes.push({
        gridRow: row,
        gridCol: col,
        medianColor: {
          r: rValues[mid],
          g: gValues[mid],
          b: bValues[mid],
        },
        boundingBox: { minX, maxX, minY, maxY },
      });
    }
  }

  // Draw detected regions
  ctx.strokeStyle = "lime";
  ctx.lineWidth = 3;
  boxes.forEach((box) => {
    const { minX, maxX, minY, maxY } = box.boundingBox;
    ctx.strokeRect(minX, minY, maxX - minX, maxY - minY);
  });

  // Map grid positions to color names based on orientation
  // Landscape layout (row-major):
  //   [0:Black] [1:White] [2:Yellow]
  //   [3:Red]   [4:Blue]  [5:Green]
  // Portrait layout (90° CW rotation):
  //   [3:Red]   [0:Black]
  //   [4:Blue]  [1:White]
  //   [5:Green] [2:Yellow]
  const colorNames = ["black", "white", "yellow", "red", "blue", "green"];
  const portraitIndexMap = [3, 0, 4, 1, 5, 2]; // Maps grid position to color index

  // Toggle LAB brightness scaling (set to false to use raw measured colors)
  const ENABLE_LAB_BRIGHTNESS_SCALING = false;

  const palette = {};

  if (ENABLE_LAB_BRIGHTNESS_SCALING) {
    // Get measured L* values for black and white
    const blackGridIdx = isPortrait ? portraitIndexMap.indexOf(0) : 0; // black is color index 0
    const whiteGridIdx = isPortrait ? portraitIndexMap.indexOf(1) : 1; // white is color index 1
    const blackBox = boxes[blackGridIdx];
    const whiteBox = boxes[whiteGridIdx];

    const [measuredBlackL] = rgbToLab(
      blackBox.medianColor.r,
      blackBox.medianColor.g,
      blackBox.medianColor.b
    );
    const [measuredWhiteL] = rgbToLab(
      whiteBox.medianColor.r,
      whiteBox.medianColor.g,
      whiteBox.medianColor.b
    );

    // Target L* values: black ~10, white ~90
    const targetBlackL = 0;
    const targetWhiteL = 100;

    // Calculate linear mapping from measured L* to target L*
    const measuredRange = measuredWhiteL - measuredBlackL;
    const targetRange = targetWhiteL - targetBlackL;

    boxes.forEach((box, gridIdx) => {
      const colorIdx = isPortrait ? portraitIndexMap[gridIdx] : gridIdx;
      const colorName = colorNames[colorIdx];

      // Convert to LAB
      const [l, a, b] = rgbToLab(box.medianColor.r, box.medianColor.g, box.medianColor.b);

      // Map L* from measured range to target range
      const normalizedL = (l - measuredBlackL) / measuredRange;
      const adjustedL = targetBlackL + normalizedL * targetRange;

      // Convert back to RGB
      const [r, g, bOut] = labToRgb(adjustedL, a, b);

      palette[colorName] = { r, g, b: bOut };
    });
  } else {
    // Use raw measured colors without adjustment
    boxes.forEach((box, gridIdx) => {
      const colorIdx = isPortrait ? portraitIndexMap[gridIdx] : gridIdx;
      const colorName = colorNames[colorIdx];
      palette[colorName] = { ...box.medianColor };
    });
  }

  return palette;
}

function clusterColors(samples) {
  const groups = [];
  const threshold = 40;

  samples.forEach((sample) => {
    let closestGroup = null;
    let minDist = Infinity;

    for (const group of groups) {
      const dr = sample.r - group.centroid.r;
      const dg = sample.g - group.centroid.g;
      const db = sample.b - group.centroid.b;
      const dist = Math.sqrt(dr * dr + dg * dg + db * db);

      if (dist < minDist && dist < threshold) {
        minDist = dist;
        closestGroup = group;
      }
    }

    if (closestGroup) {
      closestGroup.points.push(sample);
      const n = closestGroup.points.length;
      closestGroup.centroid.r = (closestGroup.centroid.r * (n - 1) + sample.r) / n;
      closestGroup.centroid.g = (closestGroup.centroid.g * (n - 1) + sample.g) / n;
      closestGroup.centroid.b = (closestGroup.centroid.b * (n - 1) + sample.b) / n;
    } else {
      groups.push({
        centroid: { r: sample.r, g: sample.g, b: sample.b },
        points: [sample],
      });
    }
  });

  // Calculate median color for each group
  groups.forEach((group) => {
    const rValues = group.points.map((p) => p.r).sort((a, b) => a - b);
    const gValues = group.points.map((p) => p.g).sort((a, b) => a - b);
    const bValues = group.points.map((p) => p.b).sort((a, b) => a - b);

    const mid = Math.floor(group.points.length / 2);
    group.medianColor = {
      r: rValues[mid],
      g: gValues[mid],
      b: bValues[mid],
    };
  });

  return groups;
}

async function saveCalibration() {
  if (!measuredPalette.value) return;

  loading.value = true;
  statusMessage.value = "Saving calibration...";
  statusType.value = "info";

  try {
    const response = await fetch(`${API_BASE}/api/settings/palette`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(measuredPalette.value),
    });

    if (response.ok) {
      statusMessage.value = "Calibration saved successfully";
      statusType.value = "success";

      // Update store palette (already updated via watcher, but ensure it's set)
      Object.assign(settingsStore.palette, measuredPalette.value);

      // Clear original palette since we're saving the new one
      originalPalette = null;

      setTimeout(() => {
        calibrationStep.value = 0;
        statusMessage.value = "";
        measuredPalette.value = null;
      }, 1500);
    } else {
      throw new Error("Failed to save calibration");
    }
  } catch (error) {
    statusMessage.value = `Error: ${error.message}`;
    statusType.value = "error";
  } finally {
    loading.value = false;
  }
}

async function resetPalette() {
  showResetDialog.value = true;
}

async function confirmResetPalette() {
  showResetDialog.value = false;
  loading.value = true;
  try {
    const response = await fetch(`${API_BASE}/api/settings/palette`, {
      method: "DELETE",
    });

    if (response.ok) {
      await settingsStore.loadPalette();
      statusMessage.value = "Palette reset to defaults";
      statusType.value = "success";
      setTimeout(() => (statusMessage.value = ""), 2000);
    } else {
      throw new Error("Failed to reset palette");
    }
  } catch (error) {
    statusMessage.value = `Error: ${error.message}`;
    statusType.value = "error";
  } finally {
    loading.value = false;
  }
}

async function savePalette() {
  loading.value = true;
  statusMessage.value = "Saving palette...";
  statusType.value = "info";

  try {
    const response = await fetch(`${API_BASE}/api/settings/palette`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(settingsStore.palette),
    });

    if (response.ok) {
      statusMessage.value = "Palette saved successfully";
      statusType.value = "success";
      setTimeout(() => (statusMessage.value = ""), 2000);
    } else {
      throw new Error("Failed to save palette");
    }
  } catch (error) {
    statusMessage.value = `Error: ${error.message}`;
    statusType.value = "error";
  } finally {
    loading.value = false;
  }
}
</script>

<template>
  <div>
    <v-dialog v-model="showResetDialog" max-width="400">
      <v-card>
        <v-card-title>Reset Palette?</v-card-title>
        <v-card-text> Reset color palette to default values? </v-card-text>
        <v-card-actions>
          <v-spacer />
          <v-btn variant="text" @click="showResetDialog = false"> Cancel </v-btn>
          <v-btn color="error" @click="confirmResetPalette"> Reset </v-btn>
        </v-card-actions>
      </v-card>
    </v-dialog>

    <v-alert type="info" variant="tonal" class="mb-4">
      Calibrate the measured color palette for your specific E6 display batch. This improves color
      accuracy during image processing.
    </v-alert>

    <!-- Current Palette Display -->
    <div v-if="calibrationStep === 0">
      <h3 class="text-h6 mb-4">Current Palette</h3>

      <v-row>
        <v-col v-for="name in colorNames" :key="name" cols="6" md="4" lg="2">
          <v-card variant="outlined">
            <div
              class="color-swatch"
              :style="{
                backgroundColor: `rgb(${settingsStore.palette[name]?.r || 0}, ${settingsStore.palette[name]?.g || 0}, ${settingsStore.palette[name]?.b || 0})`,
              }"
            />
            <v-card-text class="pa-2">
              <div class="text-subtitle-2 text-capitalize mb-2">
                {{ name }}
              </div>
              <v-text-field
                v-model.number="settingsStore.palette[name].r"
                label="R"
                type="number"
                density="compact"
                variant="outlined"
                :min="0"
                :max="255"
                class="mb-1"
              />
              <v-text-field
                v-model.number="settingsStore.palette[name].g"
                label="G"
                type="number"
                density="compact"
                variant="outlined"
                :min="0"
                :max="255"
                class="mb-1"
              />
              <v-text-field
                v-model.number="settingsStore.palette[name].b"
                label="B"
                type="number"
                density="compact"
                variant="outlined"
                :min="0"
                :max="255"
              />
            </v-card-text>
          </v-card>
        </v-col>
      </v-row>

      <v-alert v-if="statusMessage" :type="statusType" variant="tonal" class="mt-4">
        {{ statusMessage }}
      </v-alert>

      <div class="d-flex mt-4" style="gap: 8px">
        <v-btn color="primary" :loading="loading" @click="startCalibration">
          <v-icon icon="mdi-camera" start />
          Start Calibration
        </v-btn>
        <v-btn variant="outlined" :loading="loading" @click="savePalette">
          <v-icon icon="mdi-content-save" start />
          Save Palette
        </v-btn>
        <v-btn variant="text" color="error" :loading="loading" @click="resetPalette">
          Reset to Defaults
        </v-btn>
      </div>
    </div>

    <!-- Calibration Flow -->
    <v-card v-else variant="outlined" class="mt-4">
      <v-card-text>
        <!-- Step 1: Display Pattern -->
        <div v-if="calibrationStep === 1">
          <h3 class="text-h6 mb-4">Step 1: Display Calibration Image</h3>
          <p class="mb-4">
            Click the button below to display the calibration pattern on your device. The pattern
            shows 6 color boxes.
          </p>

          <v-alert v-if="statusMessage" :type="statusType" variant="tonal" class="mb-4">
            {{ statusMessage }}
          </v-alert>

          <div class="d-flex" style="gap: 8px">
            <v-btn color="primary" :loading="loading" @click="displayCalibrationPattern">
              Display Calibration Pattern
            </v-btn>
            <v-btn variant="outlined" @click="skipToPhotoStep"> Skip (Already Displayed) </v-btn>
            <v-btn variant="text" @click="cancelCalibration"> Cancel </v-btn>
          </div>
        </div>

        <!-- Step 2: Take Photo -->
        <div v-if="calibrationStep === 2">
          <h3 class="text-h6 mb-4">Step 2: Take Photo</h3>
          <p class="mb-4">Take a photo of your display showing the calibration pattern.</p>

          <v-alert type="warning" variant="tonal" class="mb-4">
            <strong>Important:</strong>
            <ul class="mt-2">
              <li>Use natural daylight (5500K) or a daylight-balanced light source</li>
              <li>Avoid shadows, reflections, and glare on the display</li>
              <li>Capture the entire display with all 6 color boxes visible</li>
              <li>Hold the camera parallel to the display (avoid angles)</li>
            </ul>
          </v-alert>

          <v-card variant="outlined" class="mb-4 pa-3">
            <p class="text-subtitle-2 mb-2">Example Photo:</p>
            <img
              :src="'/measurement_sample.jpg'"
              alt="Sample calibration photo"
              class="sample-image"
            />
            <p class="text-caption text-grey mt-2">
              Photo should be cropped to show only the display area with all 6 color boxes clearly
              visible.
            </p>
          </v-card>

          <div class="d-flex" style="gap: 8px">
            <v-btn color="primary" @click="proceedToUpload"> Continue to Upload </v-btn>
            <v-btn variant="text" @click="cancelCalibration"> Cancel </v-btn>
          </div>
        </div>

        <!-- Step 3: Upload Photo -->
        <div v-if="calibrationStep === 3">
          <h3 class="text-h6 mb-4">Step 3: Crop & Upload Photo</h3>
          <p class="mb-4">
            Crop the photo to show only the display area (remove bezels and surroundings), then
            upload it.
          </p>

          <v-alert v-if="statusMessage" :type="statusType" variant="tonal" class="mb-4">
            <div style="white-space: pre-line">{{ statusMessage }}</div>
          </v-alert>

          <v-file-input
            accept="image/*"
            label="Select calibration photo"
            variant="outlined"
            prepend-icon="mdi-camera"
            :loading="loading"
            @change="onPhotoSelected"
          />

          <canvas
            v-show="hasUploadedPhoto"
            ref="calibrationCanvas"
            class="calibration-canvas mt-4"
          />

          <div class="d-flex mt-4" style="gap: 8px">
            <v-btn variant="text" @click="cancelCalibration"> Cancel </v-btn>
          </div>
        </div>

        <!-- Step 4: Review & Save -->
        <div v-if="calibrationStep === 4">
          <h3 class="text-h6 mb-4">Step 4: Review & Save</h3>
          <p class="mb-4">
            Review the extracted colors below. You can manually adjust values if needed.
          </p>

          <v-alert v-if="statusMessage" :type="statusType" variant="tonal" class="mb-4">
            {{ statusMessage }}
          </v-alert>

          <v-row v-if="measuredPalette">
            <v-col v-for="name in colorNames" :key="name" cols="6" md="4" lg="2">
              <v-card variant="outlined">
                <div
                  class="color-swatch"
                  :style="{
                    backgroundColor: `rgb(${measuredPalette[name].r}, ${measuredPalette[name].g}, ${measuredPalette[name].b})`,
                  }"
                />
                <v-card-text class="pa-2">
                  <div class="text-subtitle-2 text-capitalize mb-2">
                    {{ name }}
                  </div>
                  <v-text-field
                    v-model.number="measuredPalette[name].r"
                    label="R"
                    type="number"
                    density="compact"
                    variant="outlined"
                    :min="0"
                    :max="255"
                    class="mb-1"
                  />
                  <v-text-field
                    v-model.number="measuredPalette[name].g"
                    label="G"
                    type="number"
                    density="compact"
                    variant="outlined"
                    :min="0"
                    :max="255"
                    class="mb-1"
                  />
                  <v-text-field
                    v-model.number="measuredPalette[name].b"
                    label="B"
                    type="number"
                    density="compact"
                    variant="outlined"
                    :min="0"
                    :max="255"
                  />
                </v-card-text>
              </v-card>
            </v-col>
          </v-row>

          <div class="d-flex mt-4" style="gap: 8px">
            <v-btn color="primary" :loading="loading" @click="saveCalibration">
              Save Calibration
            </v-btn>
            <v-btn variant="text" @click="cancelCalibration"> Cancel </v-btn>
          </div>
        </div>
      </v-card-text>
    </v-card>
  </div>
</template>

<style scoped>
.color-swatch {
  height: 60px;
  border-bottom: 1px solid rgba(0, 0, 0, 0.12);
}

.calibration-canvas {
  max-width: 100%;
  border: 1px solid #ddd;
  border-radius: 4px;
}

.sample-image {
  max-width: 100%;
  max-height: 300px;
  border: 1px solid #ddd;
  border-radius: 4px;
}
</style>
