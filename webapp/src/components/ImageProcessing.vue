<script setup>
import { ref, computed, watch, onMounted, onUnmounted } from "vue";
import { processImage, SPECTRA6, makeGrayscale16 } from "@aitjcize/epaper-image-convert";
import ToneCurve from "./ToneCurve.vue";
import CropEditor from "./CropEditor.vue";
import { useAppStore, useSettingsStore } from "../stores";
import {
  logicalFrame,
  orientCanvas,
  downscaleCanvas,
  denormalizeRect,
  rectToZoomPan,
  rectHasBars,
  loadImageFile,
  toCanvas,
} from "../utils/framing";

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
  // CSS selector to teleport the Tone Curve card into (e.g. the controls column
  // in wide-edit mode). null keeps it inline below the preview.
  toneCurveTeleport: {
    type: String,
    default: null,
  },
});

const emit = defineEmits(["processed"]);
const appStore = useAppStore();
const settingsStore = useSettingsStore();

// The preview never needs more pixels than it shows: the photo is kept at a
// bounded working size and the dithered preview is rendered at screen size.
// The upload (ImageUpload.vue) reprocesses the full-resolution photo.
const SOURCE_MAX_SIDE = 2400;
const PREVIEW_MAX_SIDE = 800;
const HISTOGRAM_MAX_SIDE = 320;

// Canvas refs
const originalCanvasRef = ref(null);
const processedCanvasRef = ref(null);

// State
const processing = ref(false);
const sliderPosition = ref(50);
const isDragging = ref(false);
const isReady = ref(false);

// Layout: how the photo lands on the frame. `scaleMode` is cover / fit /
// custom; `framing` carries the per-image adjustments made in the editor.
const scaleMode = ref("cover");
const framing = ref({ rotation: 0, flipH: false, rect: null, background: "white" });
const editorOpen = ref(false);

// Follow edits to the configured defaults live (e.g. the settings controls
// on the same page); a per-image override is simply replaced by the newer
// default the user just chose
watch(
  () => [props.params?.scaleMode, props.params?.backgroundColor],
  ([mode, bg]) => {
    if (mode) scaleMode.value = mode;
    if (bg) framing.value.background = bg;
  }
);

// Source photo: bounded working copy + the real pixel size
let sourceCanvas = null;
const sourceSize = ref(null);
let orientedCache = { key: null, canvas: null };

// Histogram data for ToneCurve (256 bins for luminance values 0-255)
const histogram = ref(null);
let processDebounceTimer = null;

// ---------------------------------------------------------------------------
// Palette
// ---------------------------------------------------------------------------
// GC16 grayscale palette built from the device's measured luminance endpoints
// (Y of black/white from /api/settings/palette), so the preview matches the
// panel. Falls back to the package defaults when the device hasn't reported.
function grayscalePalette() {
  return makeGrayscale16({
    blackY: settingsStore.palette?.black_y ?? 0.009,
    whiteY: settingsStore.palette?.white_y ?? 0.65,
    gamma: settingsStore.palette?.gamma ?? 1.42,
  });
}

// Palette pair { theoretical, perceived } for the current panel. A new object
// each time: the library's SPECTRA6 constant is never mutated.
const palettePair = computed(() => {
  if (appStore.isGrayscale) return grayscalePalette();
  const perceived =
    props.palette && Object.keys(props.palette).length > 0 ? props.palette : SPECTRA6.perceived;
  return { ...SPECTRA6, perceived };
});

// Reactive perceived palette for ToneCurve
const effectivePalette = computed(() => palettePair.value.perceived);

// ---------------------------------------------------------------------------
// Frame geometry
// ---------------------------------------------------------------------------
// The panel as the user sees it. Uses the saved/applied orientation, not the
// live dropdown, so the preview only re-lays-out when the user saves.
const frame = computed(() =>
  logicalFrame(
    appStore.systemInfo.width || 800,
    appStore.systemInfo.height || 480,
    settingsStore.appliedOrientation
  )
);

const showBackgroundToggle = computed(() => {
  if (scaleMode.value === "fit") return true;
  if (scaleMode.value !== "custom" || !framing.value.rect) return false;
  return rectHasBars(framing.value.rect, 1, 1, 0.002);
});

const rotationLabel = computed(() => {
  const r = framing.value.rotation;
  return r ? `Rotated ${r}°` : "";
});

// ---------------------------------------------------------------------------
// Histogram
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
onMounted(async () => {
  isReady.value = true;
  if (props.imageFile) {
    await loadAndProcessImage(props.imageFile);
  }
});

onUnmounted(() => {
  if (processDebounceTimer) clearTimeout(processDebounceTimer);
});

function debouncedUpdatePreview() {
  if (processDebounceTimer) clearTimeout(processDebounceTimer);
  processDebounceTimer = setTimeout(() => {
    updatePreview();
  }, 300);
}

// Watch for image file changes
watch(
  () => props.imageFile,
  async (file) => {
    if (file && isReady.value) {
      await loadAndProcessImage(file);
    }
  }
);

// Parameter changes reprocess without reloading the image. Debounced so
// dragging a slider doesn't reprocess on every tick.
watch(
  () => props.params,
  () => {
    if (sourceCanvas && isReady.value) debouncedUpdatePreview();
  },
  { deep: true }
);

// Palette or calibration edits flow straight into the preview.
watch(
  palettePair,
  async () => {
    if (sourceCanvas && isReady.value) await updatePreview();
  },
  { deep: true }
);

watch(
  () => framing.value.background,
  async () => {
    if (sourceCanvas && isReady.value) await updatePreview();
  }
);

// The frame changes shape when the saved orientation changes: a custom window
// no longer fits, so fall back to the configured layout.
watch(frame, async () => {
  framing.value.rect = null;
  if (scaleMode.value === "custom") scaleMode.value = props.params?.scaleMode || "cover";
  if (sourceCanvas && isReady.value) await updatePreview();
});

async function loadAndProcessImage(file) {
  if (!originalCanvasRef.value || !processedCanvasRef.value) return;

  processing.value = true;

  try {
    const img = await loadImageFile(file);
    sourceSize.value = { width: img.naturalWidth, height: img.naturalHeight };
    sourceCanvas = downscaleCanvas(toCanvas(img), SOURCE_MAX_SIDE);
    orientedCache = { key: null, canvas: null };

    // Default to the configured scale mode and background from the shared
    // processing params (the device settings, or the page-local params on
    // the landing-page demo); the user's per-image override below never
    // writes back to the config
    scaleMode.value = props.params?.scaleMode || "cover";
    framing.value = {
      rotation: 0,
      flipH: false,
      rect: null,
      background: props.params?.backgroundColor || "white",
    };

    await updatePreview();
  } catch (error) {
    console.error("Image loading failed:", error);
  } finally {
    processing.value = false;
  }
}

// The working photo after the editor's rotation / mirror, cached.
function orientedSource() {
  const key = `${framing.value.rotation}:${framing.value.flipH}`;
  if (orientedCache.key !== key) {
    orientedCache = {
      key,
      canvas: orientCanvas(sourceCanvas, framing.value.rotation, framing.value.flipH),
    };
  }
  return orientedCache.canvas;
}

// Layout options for processImage at a given output size.
function layoutOptions(source, outW) {
  const options = { scaleMode: scaleMode.value, backgroundColor: framing.value.background };
  if (scaleMode.value === "custom") {
    if (framing.value.rect) {
      const px = denormalizeRect(framing.value.rect, source.width, source.height);
      Object.assign(options, rectToZoomPan(px, outW));
    } else {
      options.scaleMode = "cover";
    }
  }
  return options;
}

async function updatePreview() {
  if (!sourceCanvas || !originalCanvasRef.value || !processedCanvasRef.value) return;

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

  const palette = palettePair.value;
  const source = orientedSource();

  // Preview at screen resolution, in the frame's logical orientation (no
  // orientation flag: the native-layout rotation is only for the upload).
  const { width: frameW, height: frameH } = frame.value;
  const scale = Math.min(1, PREVIEW_MAX_SIDE / Math.max(frameW, frameH));
  const previewW = Math.max(1, Math.round(frameW * scale));
  const previewH = Math.max(1, Math.round(frameH * scale));

  const result = processImage(source, {
    displayWidth: previewW,
    displayHeight: previewH,
    palette,
    params: processingParams,
    ...layoutOptions(source, previewW),
    usePerceivedOutput: true,
  });

  // Histogram of the tone-mapped (pre-dither) image, from a small copy of the
  // laid-out photo: same pixels, a fraction of the work.
  const small = downscaleCanvas(result.originalCanvas, HISTOGRAM_MAX_SIDE);
  const preDither = processImage(small, {
    displayWidth: small.width,
    displayHeight: small.height,
    palette,
    params: processingParams,
    skipDithering: true,
  });
  histogram.value = calculateHistogram(preDither.canvas);

  // Draw both layers at the preview resolution; CSS scales them to fit.
  for (const canvasRef of [originalCanvasRef, processedCanvasRef]) {
    canvasRef.value.width = previewW;
    canvasRef.value.height = previewH;
    canvasRef.value.style.width = `${previewW}px`;
    canvasRef.value.style.height = "";
  }

  // result.originalCanvas is the post-layout, pre-processing snapshot: the
  // source resized/positioned to the frame with no color processing applied.
  originalCanvasRef.value.getContext("2d").drawImage(result.originalCanvas, 0, 0);
  processedCanvasRef.value.getContext("2d").drawImage(result.canvas, 0, 0);

  emit("processed", result);
}

// ---------------------------------------------------------------------------
// Layout controls
// ---------------------------------------------------------------------------
function onModeChange(mode) {
  if (!mode) return;
  if (mode === "custom") {
    openEditor();
    return;
  }
  scaleMode.value = mode;
  updatePreview();
}

function openEditor() {
  if (!sourceCanvas) return;
  editorOpen.value = true;
}

function onEditorApply({ mode, rotation, flipH, background, rect }) {
  framing.value = { rotation, flipH, rect, background };
  scaleMode.value = mode;
  updatePreview();
}

// ---------------------------------------------------------------------------
// Before / after slider (Pointer Events: mouse, touch and pen)
// ---------------------------------------------------------------------------
function onPointerDown(event) {
  if (event.pointerType === "mouse" && event.button !== 0) return;
  isDragging.value = true;
  event.currentTarget.setPointerCapture?.(event.pointerId);
  updateSlider(event);
}

function onPointerMove(event) {
  if (isDragging.value) updateSlider(event);
}

function onPointerUp() {
  isDragging.value = false;
}

function updateSlider(event) {
  const rect = event.currentTarget.getBoundingClientRect();
  const x = event.clientX - rect.left;
  sliderPosition.value = Math.max(0, Math.min(100, (x / rect.width) * 100));
}

// Expose the layout to the upload component, which reprocesses the full
// resolution photo with the same framing.
defineExpose({
  scaleMode,
  getFraming() {
    return {
      scaleMode: scaleMode.value,
      backgroundColorName: framing.value.background,
      rotation: framing.value.rotation,
      flipH: framing.value.flipH,
      rect: framing.value.rect,
    };
  },
});
</script>

<template>
  <v-card>
    <v-card-text>
      <div class="d-flex flex-column align-center">
        <!-- Layout selector -->
        <div class="layout-bar">
          <v-btn-toggle
            :model-value="scaleMode"
            mandatory
            color="primary"
            variant="outlined"
            density="compact"
            @update:model-value="onModeChange"
          >
            <v-btn value="cover" size="small" title="Fill the frame, cropping the photo">
              <v-icon start size="small">mdi-crop-free</v-icon>
              Cover
            </v-btn>
            <v-btn value="fit" size="small" title="Show the whole photo, with bars">
              <v-icon start size="small">mdi-fit-to-screen</v-icon>
              Fit
            </v-btn>
            <v-btn value="custom" size="small" title="Choose the framing yourself">
              <v-icon start size="small">mdi-crop</v-icon>
              Adjust
            </v-btn>
          </v-btn-toggle>

          <v-btn
            v-if="scaleMode === 'custom' || framing.rotation || framing.flipH"
            size="small"
            variant="text"
            color="primary"
            prepend-icon="mdi-pencil-outline"
            @click="openEditor"
          >
            Edit framing
          </v-btn>
        </div>

        <div v-if="rotationLabel || framing.flipH" class="d-flex ga-2 mb-2">
          <v-chip v-if="rotationLabel" size="x-small" variant="tonal">{{ rotationLabel }}</v-chip>
          <v-chip v-if="framing.flipH" size="x-small" variant="tonal">Mirrored</v-chip>
        </div>

        <!-- Background color selector (only when bars are visible) -->
        <div v-if="showBackgroundToggle" class="d-flex align-center mb-3">
          <span class="text-caption text-medium-emphasis mr-2">Bars:</span>
          <v-btn-toggle
            v-model="framing.background"
            mandatory
            color="primary"
            variant="outlined"
            density="compact"
          >
            <v-btn value="white" size="small">White</v-btn>
            <v-btn value="black" size="small">Black</v-btn>
          </v-btn-toggle>
        </div>

        <div class="d-flex flex-wrap gap-4 justify-center align-end">
          <!-- Before / after comparison -->
          <div
            class="comparison-container"
            @pointerdown="onPointerDown"
            @pointermove="onPointerMove"
            @pointerup="onPointerUp"
            @pointercancel="onPointerUp"
          >
            <div class="canvas-wrapper">
              <canvas ref="originalCanvasRef" class="preview-canvas" />
              <canvas
                ref="processedCanvasRef"
                class="preview-canvas processed"
                :style="{ clipPath: `inset(0 0 0 ${sliderPosition}%)` }"
              />
              <div class="slider-line" :style="{ left: `${sliderPosition}%` }">
                <div class="slider-handle">
                  <v-icon size="small"> mdi-arrow-left-right </v-icon>
                </div>
              </div>
            </div>
            <div class="comparison-labels d-flex justify-space-between mt-2">
              <span class="text-caption">← Original</span>
              <span class="text-caption">Processed →</span>
            </div>
          </div>

          <!-- Tone Curve. In wide-edit mode it teleports to the top of the
               controls column; otherwise it stays inline below the preview.
               `defer` lets the target (rendered later in the tree) resolve. -->
          <Teleport defer :to="toneCurveTeleport" :disabled="!toneCurveTeleport">
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
          </Teleport>
        </div>

        <v-progress-linear v-if="processing" indeterminate color="primary" class="mt-2" />
      </div>
    </v-card-text>

    <CropEditor
      v-model="editorOpen"
      :source="sourceCanvas"
      :source-size="sourceSize"
      :frame="frame"
      :initial="{
        mode: scaleMode,
        rotation: framing.rotation,
        flipH: framing.flipH,
        rect: framing.rect,
        background: framing.background,
      }"
      :params="params"
      :palette="palettePair"
      @apply="onEditorApply"
    />
  </v-card>
</template>

<style scoped>
.layout-bar {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  justify-content: center;
  gap: 4px 8px;
  margin-bottom: 12px;
}

.comparison-container {
  position: relative;
  max-width: 100%;
  cursor: ew-resize;
  user-select: none;
  /* A horizontal drag moves the slider; vertical swipes still scroll the page. */
  touch-action: pan-y;
}

.canvas-wrapper {
  position: relative;
  display: inline-block;
  max-width: 100%;
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

@media (max-width: 600px) {
  .tone-curve-card {
    margin-left: 0;
    align-self: center;
  }
}
</style>
