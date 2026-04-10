<script setup>
import { ref, watch, onMounted, onUnmounted } from "vue";
import ToneCurve from "./ToneCurve.vue";
import { useAppStore, useSettingsStore } from "../stores";

const props = defineProps({
  imageFile: {
    type: File,
    default: null,
  },
  params: {
    type: Object,
    required: true,
  },
  palette: {
    type: Object,
    default: null,
  },
});

const emit = defineEmits(["processed"]);
const appStore = useAppStore();
const settingsStore = useSettingsStore();

// Canvas refs
const originalCanvasRef = ref(null);
const processedCanvasRef = ref(null);

// State
const processing = ref(false);
const sliderPosition = ref(0);
const isDragging = ref(false);

// Scale mode state
const scaleMode = ref("cover");

// Background color for uncovered areas (fit/custom modes)
// Uses perceived palette colors so dithering maps them cleanly.
const bgColorMode = ref("white"); // "black" | "white"

function getBgFillColor() {
  const p = effectivePalette.value;
  if (!p) return bgColorMode.value === "white" ? "#FFFFFF" : "#000000";
  const c = p[bgColorMode.value];
  return "#" + [c.r, c.g, c.b].map((v) => v.toString(16).padStart(2, "0")).join("");
}

// Custom mode state
const customZoom = ref(1);
const customPanX = ref(0);
const customPanY = ref(0);
const isPanning = ref(false);
let panStartX = 0;
let panStartY = 0;
let panStartImgX = 0;
let panStartImgY = 0;

// Source canvas for reprocessing
let sourceCanvas = null;
let imageProcessor = null;
let isReady = ref(false);

// Debounce timer for processing during pan/zoom
let processDebounceTimer = null;

// Reactive palette for ToneCurve (will be set after imageProcessor loads)
const effectivePalette = ref(null);

// Histogram data for ToneCurve (256 bins for luminance values 0-255)
const histogram = ref(null);

// Calculate luminance histogram from source canvas
function calculateHistogram(canvas) {
  if (!canvas) return null;

  const ctx = canvas.getContext("2d");
  const imageData = ctx.getImageData(0, 0, canvas.width, canvas.height);
  const data = imageData.data;

  const bins = new Array(256).fill(0);
  const step = Math.max(1, Math.floor(data.length / 4 / 100000));

  for (let i = 0; i < data.length; i += 4 * step) {
    const r = data[i];
    const g = data[i + 1];
    const b = data[i + 2];
    const luminance = Math.round(0.2126 * r + 0.7152 * g + 0.0722 * b);
    bins[Math.min(255, Math.max(0, luminance))]++;
  }

  const maxBin = Math.max(...bins);
  if (maxBin > 0) {
    for (let i = 0; i < bins.length; i++) {
      bins[i] = bins[i] / maxBin;
    }
  }

  return bins;
}

// Get frame dimensions based on device orientation config.
// The preview shows what the user sees on the physical device.
function getFrameDimensions() {
  let frameWidth = appStore.systemInfo.width || 800;
  let frameHeight = appStore.systemInfo.height || 480;
  const orientation = settingsStore.deviceSettings.displayOrientation;

  if (orientation === "portrait" && frameWidth > frameHeight) {
    [frameWidth, frameHeight] = [frameHeight, frameWidth];
  } else if (orientation === "landscape" && frameWidth < frameHeight) {
    [frameWidth, frameHeight] = [frameHeight, frameWidth];
  }

  return { frameWidth, frameHeight };
}

// Initialize custom mode pan/zoom to match cover position
function initCustomMode() {
  if (!sourceCanvas) return;

  const { frameWidth, frameHeight } = getFrameDimensions();
  const srcW = sourceCanvas.width;
  const srcH = sourceCanvas.height;
  const fitScale = Math.min(frameWidth / srcW, frameHeight / srcH);

  customZoom.value = fitScale;
  customPanX.value = (frameWidth - srcW * fitScale) / 2;
  customPanY.value = (frameHeight - srcH * fitScale) / 2;
}

// Get the visual-to-canvas coordinate scale from DOM
function getPreviewScale() {
  if (!originalCanvasRef.value) return 1;
  const rect = originalCanvasRef.value.getBoundingClientRect();
  if (rect.width === 0) return 1;
  return rect.width / originalCanvasRef.value.width;
}

// Quick canvas redraw during pan/zoom (no processing, immediate feedback)
function quickFrameUpdate() {
  if (!sourceCanvas || !originalCanvasRef.value || !processedCanvasRef.value) return;

  const width = originalCanvasRef.value.width;
  const height = originalCanvasRef.value.height;
  const srcW = sourceCanvas.width;
  const srcH = sourceCanvas.height;
  const w = srcW * customZoom.value;
  const h = srcH * customZoom.value;

  for (const canvasRef of [originalCanvasRef, processedCanvasRef]) {
    const ctx = canvasRef.value.getContext("2d");
    ctx.fillStyle = getBgFillColor();
    ctx.fillRect(0, 0, width, height);
    ctx.imageSmoothingEnabled = true;
    ctx.imageSmoothingQuality = "high";
    ctx.drawImage(sourceCanvas, customPanX.value, customPanY.value, w, h);
  }
}

// Debounced full processing update
function debouncedUpdatePreview() {
  if (processDebounceTimer) clearTimeout(processDebounceTimer);
  processDebounceTimer = setTimeout(() => {
    updatePreview();
  }, 300);
}

onMounted(async () => {
  try {
    imageProcessor = await import("@aitjcize/epaper-image-convert");
    isReady.value = true;

    effectivePalette.value = props.palette || imageProcessor.SPECTRA6.perceived;

    if (props.imageFile) {
      await loadAndProcessImage(props.imageFile);
    }
  } catch (error) {
    console.error("Failed to load image processor:", error);
  }
});

// Watch for image file changes
watch(
  () => props.imageFile,
  async (file) => {
    if (file && isReady.value) {
      await loadAndProcessImage(file);
    }
  }
);

// Watch for parameter changes - reprocess without reloading image
watch(
  () => props.params,
  async () => {
    if (sourceCanvas && isReady.value) {
      await updatePreview();
    }
  },
  { deep: true }
);

// Watch for palette changes - reprocess and update ToneCurve
watch(
  () => props.palette,
  async (newPalette) => {
    if (newPalette) {
      effectivePalette.value = newPalette;
    }
    if (sourceCanvas && isReady.value) {
      await updatePreview();
    }
  },
  { deep: true }
);

// Watch for background color changes
watch(bgColorMode, async () => {
  if (sourceCanvas && isReady.value) {
    await updatePreview();
  }
});

// Watch for scale mode changes
watch(scaleMode, async (newMode) => {
  if (newMode === "custom") {
    initCustomMode();
  }
  if (sourceCanvas && isReady.value) {
    await updatePreview();
  }
});

async function loadAndProcessImage(file) {
  if (!originalCanvasRef.value || !processedCanvasRef.value) return;

  processing.value = true;

  try {
    const img = await loadImage(file);

    sourceCanvas = document.createElement("canvas");
    sourceCanvas.width = img.width;
    sourceCanvas.height = img.height;
    const sourceCtx = sourceCanvas.getContext("2d");
    sourceCtx.drawImage(img, 0, 0);

    // Auto-select scale mode: use fit if source orientation doesn't match
    // display orientation, cover otherwise
    const { frameWidth, frameHeight } = getFrameDimensions();
    const isSourcePortrait = img.height > img.width;
    const isTargetPortrait = frameHeight > frameWidth;
    if (isSourcePortrait !== isTargetPortrait) {
      scaleMode.value = "fit";
    } else {
      scaleMode.value = "cover";
    }

    // Reinitialize custom mode if active
    if (scaleMode.value === "custom") {
      initCustomMode();
    }

    await updatePreview();
  } catch (error) {
    console.error("Image loading failed:", error);
  } finally {
    processing.value = false;
  }
}

async function updatePreview() {
  if (!sourceCanvas || !originalCanvasRef.value || !processedCanvasRef.value || !imageProcessor)
    return;

  const processingParams = {
    exposure: props.params.exposure,
    saturation: props.params.saturation,
    toneMode: props.params.toneMode,
    contrast: props.params.contrast,
    strength: props.params.strength,
    shadowBoost: props.params.shadowBoost,
    highlightCompress: props.params.highlightCompress,
    midpoint: props.params.midpoint,
    colorMethod: props.params.colorMethod,
    ditherAlgorithm: props.params.ditherAlgorithm,
    compressDynamicRange: props.params.compressDynamicRange,
  };

  const palette = imageProcessor.SPECTRA6;
  if (props.palette && Object.keys(props.palette).length > 0) {
    palette.perceived = props.palette;
  }

  const { frameWidth, frameHeight } = getFrameDimensions();

  // For preview, pass oriented frame dimensions directly (no orientation
  // flag — we don't want the native-layout rotation that the upload needs).
  const commonOpts = {
    displayWidth: frameWidth,
    displayHeight: frameHeight,
    palette,
    params: processingParams,
    scaleMode: scaleMode.value,
    backgroundColor: bgColorMode.value,
    zoom: customZoom.value,
    panX: customPanX.value,
    panY: customPanY.value,
  };

  // Process with dithering (perceived palette for preview)
  const result = imageProcessor.processImage(sourceCanvas, {
    ...commonOpts,
    usePerceivedOutput: true,
  });

  // Process without dithering for histogram
  const preDitherResult = imageProcessor.processImage(sourceCanvas, {
    ...commonOpts,
    skipDithering: true,
  });
  histogram.value = calculateHistogram(preDitherResult.canvas);

  // Update canvas dimensions
  const actualWidth = result.canvas.width;
  const actualHeight = result.canvas.height;
  originalCanvasRef.value.width = actualWidth;
  originalCanvasRef.value.height = actualHeight;
  processedCanvasRef.value.width = actualWidth;
  processedCanvasRef.value.height = actualHeight;

  // Scale down the visual size if larger than 800px while maintaining aspect ratio
  const MAX_PREVIEW_SIZE = 800;
  let styleWidth = actualWidth;
  let styleHeight = actualHeight;

  if (styleWidth > MAX_PREVIEW_SIZE || styleHeight > MAX_PREVIEW_SIZE) {
    const ratio = Math.min(MAX_PREVIEW_SIZE / styleWidth, MAX_PREVIEW_SIZE / styleHeight);
    styleWidth = Math.round(styleWidth * ratio);
    styleHeight = Math.round(styleHeight * ratio);
  }

  if (originalCanvasRef.value) {
    originalCanvasRef.value.style.width = `${styleWidth}px`;
    originalCanvasRef.value.style.height = "";
  }
  if (processedCanvasRef.value) {
    processedCanvasRef.value.style.width = `${styleWidth}px`;
    processedCanvasRef.value.style.height = "";
  }

  // Draw original (pre-dither result has same scaling but no dithering)
  const originalCtx = originalCanvasRef.value.getContext("2d");
  originalCtx.drawImage(preDitherResult.canvas, 0, 0);

  // Draw processed result (with dithering)
  const processedCtx = processedCanvasRef.value.getContext("2d");
  processedCtx.drawImage(result.canvas, 0, 0);

  emit("processed", result);
}

function loadImage(file) {
  return new Promise((resolve, reject) => {
    const img = new Image();
    img.onload = () => resolve(img);
    img.onerror = reject;
    img.src = URL.createObjectURL(file);
  });
}

// Mouse event handlers
function onMouseDown(event) {
  if (scaleMode.value === "custom") {
    isPanning.value = true;
    panStartX = event.clientX;
    panStartY = event.clientY;
    panStartImgX = customPanX.value;
    panStartImgY = customPanY.value;
    event.preventDefault();
  } else {
    isDragging.value = true;
    updateSlider(event);
  }
}

function onMouseMove(event) {
  if (isPanning.value && scaleMode.value === "custom") {
    const scale = getPreviewScale();
    const dx = (event.clientX - panStartX) / scale;
    const dy = (event.clientY - panStartY) / scale;
    customPanX.value = panStartImgX + dx;
    customPanY.value = panStartImgY + dy;
    quickFrameUpdate();
    debouncedUpdatePreview();
  } else if (isDragging.value) {
    updateSlider(event);
  }
}

function onMouseUp() {
  if (isPanning.value) {
    isPanning.value = false;
    if (processDebounceTimer) {
      clearTimeout(processDebounceTimer);
      processDebounceTimer = null;
    }
    updatePreview();
  }
  isDragging.value = false;
}

function onWheel(event) {
  if (scaleMode.value !== "custom" || !sourceCanvas) return;
  event.preventDefault();

  const { frameWidth, frameHeight } = getFrameDimensions();
  const srcW = sourceCanvas.width;
  const srcH = sourceCanvas.height;
  const fitScale = Math.min(frameWidth / srcW, frameHeight / srcH);
  const maxZoomVal = Math.max(frameWidth / srcW, frameHeight / srcH) * 5;

  const zoomFactor = event.deltaY > 0 ? 0.95 : 1.05;
  let newZoom = customZoom.value * zoomFactor;
  newZoom = Math.max(fitScale * 0.25, Math.min(maxZoomVal, newZoom));

  // Zoom around mouse position
  const scale = getPreviewScale();
  const rect = event.currentTarget.getBoundingClientRect();
  const mouseFrameX = (event.clientX - rect.left) / scale;
  const mouseFrameY = (event.clientY - rect.top) / scale;

  const zoomRatio = newZoom / customZoom.value;
  customPanX.value = mouseFrameX - (mouseFrameX - customPanX.value) * zoomRatio;
  customPanY.value = mouseFrameY - (mouseFrameY - customPanY.value) * zoomRatio;
  customZoom.value = newZoom;

  quickFrameUpdate();
  debouncedUpdatePreview();
}

function updateSlider(event) {
  const container = event.currentTarget;
  const rect = container.getBoundingClientRect();
  const x = event.clientX - rect.left;
  sliderPosition.value = Math.max(0, Math.min(100, (x / rect.width) * 100));
}

// Expose method for upload component to get framed canvas and background mask
defineExpose({
  scaleMode,
  getUploadParams() {
    return {
      backgroundColorName: bgColorMode.value,
      zoom: customZoom.value,
      panX: customPanX.value,
      panY: customPanY.value,
    };
  },
});

onUnmounted(() => {
  if (processDebounceTimer) clearTimeout(processDebounceTimer);
});
</script>

<template>
  <v-card>
    <v-card-text>
      <div class="d-flex flex-column align-center">
        <!-- Scale Mode Selector -->
        <v-btn-toggle
          v-model="scaleMode"
          mandatory
          color="primary"
          variant="outlined"
          density="compact"
          class="mb-3"
        >
          <v-btn value="cover" size="small">
            <v-icon start size="small">mdi-crop-free</v-icon>
            Cover
          </v-btn>
          <v-btn value="fit" size="small">
            <v-icon start size="small">mdi-fit-to-screen</v-icon>
            Fit
          </v-btn>
          <v-btn value="custom" size="small">
            <v-icon start size="small">mdi-cursor-move</v-icon>
            Custom
          </v-btn>
        </v-btn-toggle>

        <!-- Background color selector (only for fit/custom modes) -->
        <div v-if="scaleMode !== 'cover'" class="d-flex align-center mb-3">
          <span class="text-caption text-medium-emphasis mr-2">Background:</span>
          <v-btn-toggle
            v-model="bgColorMode"
            mandatory
            color="primary"
            variant="outlined"
            density="compact"
          >
            <v-btn value="black" size="small">Black</v-btn>
            <v-btn value="white" size="small">White</v-btn>
          </v-btn-toggle>
        </div>

        <div class="d-flex flex-wrap gap-4 justify-center align-end">
          <!-- Comparison / Custom Container -->
          <div
            class="comparison-container"
            :class="{ 'custom-mode': scaleMode === 'custom' }"
            @mousedown="onMouseDown"
            @mousemove="onMouseMove"
            @mouseup="onMouseUp"
            @mouseleave="onMouseUp"
            @wheel="onWheel"
          >
            <div class="canvas-wrapper">
              <canvas ref="originalCanvasRef" class="preview-canvas" />
              <canvas
                ref="processedCanvasRef"
                class="preview-canvas processed"
                :style="{
                  clipPath: scaleMode !== 'custom' ? `inset(0 0 0 ${sliderPosition}%)` : 'none',
                }"
              />
              <!-- Comparison slider (hidden in custom mode) -->
              <div
                v-if="scaleMode !== 'custom'"
                class="slider-line"
                :style="{ left: `${sliderPosition}%` }"
              >
                <div class="slider-handle">
                  <v-icon size="small"> mdi-arrow-left-right </v-icon>
                </div>
              </div>
            </div>
            <div class="comparison-labels d-flex justify-space-between mt-2">
              <template v-if="scaleMode !== 'custom'">
                <span class="text-caption">← Original</span>
                <span class="text-caption">Processed →</span>
              </template>
              <span v-else class="text-caption text-medium-emphasis">
                Drag to pan, scroll to zoom
              </span>
            </div>
          </div>

          <!-- Tone Curve -->
          <v-card variant="outlined" class="tone-curve-card">
            <v-card-subtitle class="pt-2"> Tone Curve </v-card-subtitle>
            <div class="d-flex justify-center pa-4">
              <ToneCurve
                :params="params"
                :palette="effectivePalette"
                :histogram="histogram"
                class="curve-canvas"
              />
            </div>
          </v-card>
        </div>

        <v-progress-linear v-if="processing" indeterminate color="primary" class="mt-2" />
      </div>
    </v-card-text>
  </v-card>
</template>

<style scoped>
.comparison-container {
  position: relative;
  cursor: ew-resize;
  user-select: none;
}

.comparison-container.custom-mode {
  cursor: grab;
}

.comparison-container.custom-mode:active {
  cursor: grabbing;
}

.canvas-wrapper {
  position: relative;
  display: inline-block;
  background: #f5f5f5;
  border-radius: 8px;
  overflow: hidden;
}

.preview-canvas {
  display: block;
  max-width: 100%;
  height: auto;
}

.preview-canvas.processed {
  position: absolute;
  top: 0;
  left: 0;
  z-index: 1;
}

.slider-line {
  position: absolute;
  top: 0;
  bottom: 0;
  width: 3px;
  background: white;
  z-index: 2;
  transform: translateX(-50%);
  box-shadow: 0 0 4px rgba(0, 0, 0, 0.3);
}

.slider-handle {
  position: absolute;
  top: 50%;
  left: 50%;
  transform: translate(-50%, -50%);
  width: 32px;
  height: 32px;
  background: white;
  border-radius: 50%;
  display: flex;
  align-items: center;
  justify-content: center;
  box-shadow: 0 2px 8px rgba(0, 0, 0, 0.2);
}

.curve-canvas {
  border: 1px solid #e0e0e0;
  border-radius: 4px;
}

.tone-curve-card {
  flex-shrink: 0;
  align-self: flex-end;
  margin-left: 20px;
}
</style>
