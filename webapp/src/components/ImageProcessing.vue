<script setup>
import { ref, watch, onMounted, onUnmounted } from "vue";
import ToneCurve from "./ToneCurve.vue";
import { useAppStore } from "../stores";

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

// Canvas refs
const originalCanvasRef = ref(null);
const processedCanvasRef = ref(null);

// State
const processing = ref(false);
const sliderPosition = ref(0);
const isDragging = ref(false);

// Source canvas for reprocessing
let sourceCanvas = null;
let imageProcessor = null;
let isReady = ref(false);

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

  // Initialize 256 bins
  const bins = new Array(256).fill(0);

  // Sample pixels (skip some for performance on large images)
  const step = Math.max(1, Math.floor(data.length / 4 / 100000)); // Max ~100k samples

  for (let i = 0; i < data.length; i += 4 * step) {
    const r = data[i];
    const g = data[i + 1];
    const b = data[i + 2];

    // Calculate perceived luminance (same formula as in ToneCurve)
    const luminance = Math.round(0.2126 * r + 0.7152 * g + 0.0722 * b);
    bins[Math.min(255, Math.max(0, luminance))]++;
  }

  // Normalize to 0-1 range (relative to max bin)
  const maxBin = Math.max(...bins);
  if (maxBin > 0) {
    for (let i = 0; i < bins.length; i++) {
      bins[i] = bins[i] / maxBin;
    }
  }

  return bins;
}

onMounted(async () => {
  // Load the image processing library
  try {
    imageProcessor = await import("@aitjcize/epaper-image-convert");
    isReady.value = true;

    // Set effective palette for ToneCurve (use props.palette if provided, otherwise SPECTRA6.perceived)
    effectivePalette.value = props.palette || imageProcessor.SPECTRA6.perceived;

    // Process if file was already set
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

async function loadAndProcessImage(file) {
  if (!originalCanvasRef.value || !processedCanvasRef.value) return;

  processing.value = true;

  try {
    // Load image with EXIF orientation applied
    const img = await loadImage(file);

    // Create source canvas from loaded image
    sourceCanvas = document.createElement("canvas");
    sourceCanvas.width = img.width;
    sourceCanvas.height = img.height;
    const sourceCtx = sourceCanvas.getContext("2d");
    sourceCtx.drawImage(img, 0, 0);

    // Process and display
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

  // Build palette for processing
  const palette = imageProcessor.SPECTRA6;
  if (props.palette && Object.keys(props.palette).length > 0) {
    palette.perceived = props.palette;
  }

  // Use native hardware dimensions for processing
  const targetWidth = appStore.systemInfo.width;
  const targetHeight = appStore.systemInfo.height;

  const result = imageProcessor.processImage(sourceCanvas, {
    displayWidth: targetWidth,
    displayHeight: targetHeight,
    palette,
    params: processingParams,
    skipRotation: true, // Keep original orientation for "natural" preview
    usePerceivedOutput: true,
  });

  // Process again without dithering to get histogram of tone-mapped image
  const preDitherResult = imageProcessor.processImage(sourceCanvas, {
    displayWidth: targetWidth,
    displayHeight: targetHeight,
    palette,
    params: processingParams,
    skipRotation: true,
    skipDithering: true,
  });
  histogram.value = calculateHistogram(preDitherResult.canvas);

  // Update canvas dimensions to match the processed result
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

  // Update logic to set style on the canvas elements directly or wrapper
  if (originalCanvasRef.value) {
    originalCanvasRef.value.style.width = `${styleWidth}px`;
    originalCanvasRef.value.style.height = `${styleHeight}px`;
  }
  if (processedCanvasRef.value) {
    processedCanvasRef.value.style.width = `${styleWidth}px`;
    processedCanvasRef.value.style.height = `${styleHeight}px`;
  }

  // Draw original using cover mode (same as old webapp)
  const originalResized = imageProcessor.resizeImageCover(sourceCanvas, actualWidth, actualHeight);
  const originalCtx = originalCanvasRef.value.getContext("2d");
  originalCtx.drawImage(originalResized, 0, 0);

  // Draw processed result
  const processedCtx = processedCanvasRef.value.getContext("2d");
  processedCtx.drawImage(result.canvas, 0, 0, actualWidth, actualHeight);

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

// Slider drag handling
function startDrag(event) {
  isDragging.value = true;
  updateSlider(event);
}

function onDrag(event) {
  if (isDragging.value) {
    updateSlider(event);
  }
}

function stopDrag() {
  isDragging.value = false;
}

function updateSlider(event) {
  const container = event.currentTarget;
  const rect = container.getBoundingClientRect();
  const x = event.clientX - rect.left;
  sliderPosition.value = Math.max(0, Math.min(100, (x / rect.width) * 100));
}

onUnmounted(() => {
  // Cleanup
});
</script>

<template>
  <v-card>
    <v-card-text>
      <div class="d-flex flex-column align-center">
        <div class="d-flex flex-wrap gap-4 justify-center align-end">
          <!-- Comparison Slider -->
          <div
            class="comparison-container"
            @mousedown="startDrag"
            @mousemove="onDrag"
            @mouseup="stopDrag"
            @mouseleave="stopDrag"
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
