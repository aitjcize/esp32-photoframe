<script setup>
import { ref, onMounted, watch, nextTick, useAttrs } from "vue";

defineOptions({ inheritAttrs: false });

const props = defineProps({
  params: {
    type: Object,
    required: true,
  },
  size: {
    type: Number,
    default: 200,
  },
  palette: {
    type: Object,
    default: null,
  },
  histogram: {
    type: Array,
    default: null,
  },
});

const attrs = useAttrs();
const canvasRef = ref(null);

// Simple RGB to LAB L* conversion (approximate)
function rgbToL(r, g, b) {
  // Convert to linear RGB
  const toLinear = (c) => {
    c = c / 255;
    return c <= 0.04045 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4);
  };
  const lr = toLinear(r);
  const lg = toLinear(g);
  const lb = toLinear(b);

  // Convert to Y (luminance)
  const y = 0.2126 * lr + 0.7152 * lg + 0.0722 * lb;

  // Convert Y to L*
  const yn = 1.0; // Reference white Y
  const fy = y / yn > 0.008856 ? Math.pow(y / yn, 1 / 3) : ((903.3 * y) / yn + 16) / 116;
  return 116 * fy - 16;
}

function drawToneCurve() {
  if (!canvasRef.value) return;

  const canvas = canvasRef.value;
  const ctx = canvas.getContext("2d");
  const size = props.size;

  canvas.width = size;
  canvas.height = size;

  ctx.fillStyle = "#f5f5f5";
  ctx.fillRect(0, 0, size, size);

  // Draw histogram if available (behind everything)
  if (props.histogram && props.histogram.length === 256) {
    ctx.fillStyle = "rgba(180, 180, 180, 0.4)";
    ctx.beginPath();
    ctx.moveTo(0, size);

    for (let i = 0; i < 256; i++) {
      const x = (i / 255) * size;
      const height = props.histogram[i] * size * 0.8; // Scale to 80% of canvas height
      const y = size - height;
      ctx.lineTo(x, y);
    }

    ctx.lineTo(size, size);
    ctx.closePath();
    ctx.fill();
  }

  ctx.strokeStyle = "#e0e0e0";
  ctx.lineWidth = 1;
  for (let i = 0; i <= 4; i++) {
    const pos = (i / 4) * size;
    ctx.beginPath();
    ctx.moveTo(pos, 0);
    ctx.lineTo(pos, size);
    ctx.stroke();
    ctx.beginPath();
    ctx.moveTo(0, pos);
    ctx.lineTo(size, pos);
    ctx.stroke();
  }

  ctx.strokeStyle = "#bdbdbd";
  ctx.beginPath();
  ctx.moveTo(0, size);
  ctx.lineTo(size, 0);
  ctx.stroke();

  ctx.strokeStyle = "#1976D2";
  ctx.lineWidth = 2;
  ctx.beginPath();

  const params = props.params;

  // Calculate dynamic range compression bounds if enabled
  let blackL = 0;
  let whiteL = 100;
  if (params.compressDynamicRange && props.palette) {
    const black = props.palette.black || { r: 0, g: 0, b: 0 };
    const white = props.palette.white || { r: 255, g: 255, b: 255 };
    blackL = rgbToL(black.r, black.g, black.b);
    whiteL = rgbToL(white.r, white.g, white.b);
  }

  for (let x = 0; x <= size; x++) {
    const input = x / size;
    let output;

    if (params.toneMode === "scurve") {
      const strength = params.strength;
      const midpoint = params.midpoint;
      const shadowBoost = params.shadowBoost;
      const highlightCompress = params.highlightCompress;

      if (strength === 0) {
        output = input;
      } else if (input <= midpoint) {
        const shadowVal = input / midpoint;
        output = Math.pow(shadowVal, 1.0 - strength * shadowBoost) * midpoint;
      } else {
        const highlightVal = (input - midpoint) / (1.0 - midpoint);
        output =
          midpoint + Math.pow(highlightVal, 1.0 + strength * highlightCompress) * (1.0 - midpoint);
      }
    } else {
      const contrast = params.contrast;
      output = ((input * 255.0 - 128.0) * contrast + 128.0) / 255.0;
    }

    output = Math.max(0, Math.min(1, output));

    // Apply dynamic range compression if enabled
    if (params.compressDynamicRange && props.palette) {
      // Map output from 0-1 to blackL-whiteL range (normalized to 0-1)
      output = (blackL + output * (whiteL - blackL)) / 100;
    }

    output = Math.max(0, Math.min(1, output));
    const y = size - output * size;

    if (x === 0) {
      ctx.moveTo(x, y);
    } else {
      ctx.lineTo(x, y);
    }
  }

  ctx.stroke();
}

function scheduleDraw() {
  requestAnimationFrame(() => drawToneCurve());
}

onMounted(async () => {
  await nextTick();
  scheduleDraw();
});

watch(
  () => props.params,
  async () => {
    await nextTick();
    scheduleDraw();
  },
  { deep: true }
);

watch(
  () => props.palette,
  async () => {
    await nextTick();
    scheduleDraw();
  },
  { deep: true }
);

watch(
  () => props.histogram,
  async () => {
    await nextTick();
    scheduleDraw();
  }
);
</script>

<template>
  <canvas ref="canvasRef" v-bind="attrs" />
</template>
