<script setup>
// Manual framing editor: the whole photo is shown with a frame-shaped window
// over it. Every gesture only moves DOM boxes (CSS transforms); the dithered
// "on the frame" preview is recomputed on a small canvas, throttled, and the
// heavy full-size processing happens once, after Apply, in the parent.
import { ref, computed, watch, nextTick, onBeforeUnmount } from "vue";
import { useDisplay } from "vuetify";
import { processImage } from "@aitjcize/epaper-image-convert";
import {
  orientCanvas,
  downscaleCanvas,
  orientedSize,
  coverRect,
  fitRect,
  clampRect,
  rectLimits,
  rectHasBars,
  upscaleFactor,
  normalizeRect,
  denormalizeRect,
  rectToZoomPan,
  rectsEqual,
  normalizeRotation,
} from "../utils/framing";

const props = defineProps({
  modelValue: { type: Boolean, default: false },
  // Unrotated source canvas (may already be downscaled for previews).
  source: { type: Object, default: null },
  // Pixel size of the original photo (before any downscale), for the
  // enlargement indicator. Defaults to the source canvas size.
  sourceSize: { type: Object, default: null },
  // Logical frame size, i.e. the panel as the user sees it.
  frame: { type: Object, required: true },
  // Starting point: { mode, rotation, flipH, rect (normalized), background }
  initial: { type: Object, default: null },
  params: { type: Object, required: true },
  // Palette pair { theoretical, perceived }
  palette: { type: Object, required: true },
});

const emit = defineEmits(["update:modelValue", "apply"]);

const { smAndDown } = useDisplay();

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
const EDIT_MAX_SIDE = 1600; // px, on-screen copy of the photo
const PREVIEW_LONG_SIDE = 320; // px, "on the frame" dithered preview
const STAGE_PADDING = 0.06; // fraction of the stage kept clear around the content

const rotation = ref(0);
const flipH = ref(false);
const background = ref("white");
const rect = ref({ x: 0, y: 0, w: 1, h: 1 }); // edit-canvas pixels
const stageSize = ref({ w: 0, h: 0 });
const view = ref({ k: 1, ox: 0, oy: 0 }); // edit px -> stage px
const animate = ref(false); // smooth view changes when not mid-gesture
const gestureActive = ref(false);
const activeHandle = ref(null);
const pipHidden = ref(false);
const previewBusy = ref(false);

const stageRef = ref(null);
const imageHostRef = ref(null);
const previewRef = ref(null);
const pipRef = ref(null);

let editCanvas = null; // oriented, downscaled copy of props.source
const editSize = ref({ w: 1, h: 1 });

const aspect = computed(() => props.frame.width / props.frame.height);
const frameIsPortrait = computed(() => props.frame.height > props.frame.width);

// Edit-canvas pixels per full-resolution pixel: the enlargement indicator must
// be measured against the real photo, not the downscaled copy.
const editScale = computed(() => {
  const full = props.sourceSize || props.source;
  if (!full) return 1;
  const oriented = orientedSize(full.width, full.height, rotation.value);
  return editSize.value.w / oriented.width;
});

const limits = computed(() =>
  rectLimits(
    editSize.value.w,
    editSize.value.h,
    props.frame.width,
    props.frame.height,
    editScale.value
  )
);

const hasBars = computed(() => rectHasBars(rect.value, editSize.value.w, editSize.value.h));
const enlargement = computed(() => upscaleFactor(rect.value, props.frame.width, editScale.value));
const enlargementLabel = computed(() =>
  enlargement.value > 1.1 ? `Enlarged ×${enlargement.value.toFixed(1)}` : ""
);
const enlargementSevere = computed(() => enlargement.value > 2);

const backgroundHex = computed(() => {
  const c = props.palette?.perceived?.[background.value];
  if (!c) return background.value === "black" ? "#000000" : "#ffffff";
  return `rgb(${c.r}, ${c.g}, ${c.b})`;
});

// ---------------------------------------------------------------------------
// Geometry helpers
// ---------------------------------------------------------------------------
function clamp(r) {
  return clampRect(r, editSize.value.w, editSize.value.h, aspect.value, limits.value);
}

function coverPreset() {
  return coverRect(editSize.value.w, editSize.value.h, props.frame.width, props.frame.height);
}

function fitPreset() {
  return fitRect(editSize.value.w, editSize.value.h, props.frame.width, props.frame.height);
}

// Stage pixel -> edit-canvas pixel, using the current (possibly frozen) view.
function toImage(clientX, clientY) {
  const bounds = stageRef.value.getBoundingClientRect();
  const { k, ox, oy } = view.value;
  return { x: (clientX - bounds.left - ox) / k, y: (clientY - bounds.top - oy) / k };
}

// Fit the union of the photo and the window into the stage.
function refreshView(withAnimation = true) {
  const { w: sw, h: sh } = stageSize.value;
  if (!sw || !sh) return;
  const r = rect.value;
  const x0 = Math.min(0, r.x);
  const y0 = Math.min(0, r.y);
  const x1 = Math.max(editSize.value.w, r.x + r.w);
  const y1 = Math.max(editSize.value.h, r.y + r.h);
  const ww = x1 - x0;
  const wh = y1 - y0;
  const k = Math.min(sw / (ww * (1 + 2 * STAGE_PADDING)), sh / (wh * (1 + 2 * STAGE_PADDING)));
  animate.value = withAnimation;
  view.value = { k, ox: (sw - ww * k) / 2 - x0 * k, oy: (sh - wh * k) / 2 - y0 * k };
}

const imageStyle = computed(() => ({
  width: `${editSize.value.w}px`,
  height: `${editSize.value.h}px`,
  transform: `translate(${view.value.ox}px, ${view.value.oy}px) scale(${view.value.k})`,
}));

const boxStyle = computed(() => {
  const { k, ox, oy } = view.value;
  const r = rect.value;
  return {
    transform: `translate(${ox + r.x * k}px, ${oy + r.y * k}px)`,
    width: `${r.w * k}px`,
    height: `${r.h * k}px`,
  };
});

// ---------------------------------------------------------------------------
// Source handling
// ---------------------------------------------------------------------------
function rebuildEditCanvas() {
  if (!props.source) return;
  const oriented = orientCanvas(props.source, rotation.value, flipH.value);
  editCanvas = downscaleCanvas(oriented, EDIT_MAX_SIDE);
  editSize.value = { w: editCanvas.width, h: editCanvas.height };
  attachEditCanvas();
}

// The dialog body mounts lazily; (re)attach the on-screen canvas to its host.
function attachEditCanvas() {
  const host = imageHostRef.value;
  if (host && editCanvas && host.firstChild !== editCanvas) {
    host.replaceChildren(editCanvas);
  }
}

function seedFromInitial() {
  const init = props.initial || {};
  rotation.value = normalizeRotation(init.rotation);
  flipH.value = !!init.flipH;
  background.value = init.background === "black" ? "black" : "white";
  rebuildEditCanvas();
  if (init.rect && init.mode === "custom") {
    rect.value = clamp(denormalizeRect(init.rect, editSize.value.w, editSize.value.h));
  } else if (init.mode === "fit") {
    rect.value = clamp(fitPreset());
  } else {
    rect.value = clamp(coverPreset());
  }
}

// ---------------------------------------------------------------------------
// Open / close
// ---------------------------------------------------------------------------
let resizeObserver = null;

watch(
  () => props.modelValue,
  async (open) => {
    if (!open) {
      teardownStage();
      return;
    }
    await nextTick();
    seedFromInitial();
    setupStage();
    await nextTick();
    attachEditCanvas();
    measureStage();
    refreshView(false);
    schedulePreview(0);
    requestAnimationFrame(() => stageRef.value?.focus({ preventScroll: true }));
  }
);

function setupStage() {
  if (!stageRef.value || resizeObserver) return;
  resizeObserver = new ResizeObserver(() => {
    measureStage();
    refreshView(false);
  });
  resizeObserver.observe(stageRef.value);
}

function teardownStage() {
  resizeObserver?.disconnect();
  resizeObserver = null;
  clearTimeout(previewTimer);
  previewTimer = null;
  pointers.clear();
  gesture = null;
  gestureActive.value = false;
}

function measureStage() {
  const el = stageRef.value;
  if (!el) return;
  stageSize.value = { w: el.clientWidth, h: el.clientHeight };
}

onBeforeUnmount(teardownStage);

function close() {
  emit("update:modelValue", false);
}

function apply() {
  const r = rect.value;
  let mode = "custom";
  if (rectsEqual(r, coverPreset(), 0.75)) mode = "cover";
  else if (rectsEqual(r, fitPreset(), 0.75)) mode = "fit";
  emit("apply", {
    mode,
    rotation: rotation.value,
    flipH: flipH.value,
    background: background.value,
    rect: normalizeRect(r, editSize.value.w, editSize.value.h),
  });
  close();
}

// ---------------------------------------------------------------------------
// Presets and orientation
// ---------------------------------------------------------------------------
function setRect(r, withAnimation = true) {
  rect.value = clamp(r);
  refreshView(withAnimation);
  schedulePreview();
}

function useCover() {
  setRect(coverPreset());
}

function useFit() {
  setRect(fitPreset());
}

function rotate(delta) {
  const before = rect.value;
  const { w: ew, h: eh } = editSize.value;
  // Rotate the window's centre with the photo and keep its area, so the
  // framing survives a quarter turn as closely as the fixed aspect allows.
  const cx = before.x + before.w / 2;
  const cy = before.y + before.h / 2;
  const center = delta > 0 ? { x: eh - cy, y: cx } : { x: cy, y: ew - cx }; // in the rotated photo
  const area = before.w * before.h;
  rotation.value = normalizeRotation(rotation.value + delta);
  rebuildEditCanvas();
  const w = Math.sqrt(area * aspect.value);
  const h = w / aspect.value;
  setRect({ x: center.x - w / 2, y: center.y - h / 2, w, h });
}

function mirror() {
  const before = rect.value;
  flipH.value = !flipH.value;
  rebuildEditCanvas();
  setRect({ ...before, x: editSize.value.w - (before.x + before.w) });
}

watch(background, () => schedulePreview(0));

// ---------------------------------------------------------------------------
// Gestures (Pointer Events: mouse, touch and pen share one code path)
// ---------------------------------------------------------------------------
const pointers = new Map(); // pointerId -> { x, y } (client px)
let gesture = null;
let lastTap = { time: 0, x: 0, y: 0 };

function onPointerDown(event) {
  if (event.pointerType === "mouse" && event.button !== 0) return;
  event.preventDefault();
  stageRef.value.setPointerCapture?.(event.pointerId);
  pointers.set(event.pointerId, { x: event.clientX, y: event.clientY });

  if (pointers.size === 1) {
    if (isDoubleTap(event)) {
      pointers.delete(event.pointerId);
      useCover();
      return;
    }
    const handle = event.target.closest?.("[data-handle]")?.dataset.handle || null;
    startGesture(handle ? "resize" : "move", event, handle);
  } else if (pointers.size === 2) {
    startPinch();
  }
}

function isDoubleTap(event) {
  const now = performance.now();
  const near = Math.abs(event.clientX - lastTap.x) < 24 && Math.abs(event.clientY - lastTap.y) < 24;
  const double = now - lastTap.time < 320 && near;
  lastTap = double ? { time: 0, x: 0, y: 0 } : { time: now, x: event.clientX, y: event.clientY };
  return double;
}

function startGesture(type, event, handle) {
  animate.value = false; // the view stays frozen while a finger is down
  gestureActive.value = true;
  activeHandle.value = handle;
  const start = toImage(event.clientX, event.clientY);
  const r = rect.value;
  const anchor = handle
    ? {
        x: handle.includes("w") ? r.x + r.w : r.x,
        y: handle.includes("n") ? r.y + r.h : r.y,
      }
    : null;
  gesture = { type, handle, start, startRect: { ...r }, anchor };
}

function startPinch() {
  const [a, b] = [...pointers.values()];
  const mid = toImage((a.x + b.x) / 2, (a.y + b.y) / 2);
  gestureActive.value = true;
  activeHandle.value = null;
  gesture = {
    type: "pinch",
    startDistance: Math.hypot(a.x - b.x, a.y - b.y) || 1,
    startMid: mid,
    startRect: { ...rect.value },
  };
}

function onPointerMove(event) {
  if (!pointers.has(event.pointerId) || !gesture) return;
  pointers.set(event.pointerId, { x: event.clientX, y: event.clientY });

  if (gesture.type === "pinch") {
    if (pointers.size < 2) return;
    const [a, b] = [...pointers.values()];
    const distance = Math.hypot(a.x - b.x, a.y - b.y) || 1;
    const mid = toImage((a.x + b.x) / 2, (a.y + b.y) / 2);
    const r0 = gesture.startRect;
    // Direct manipulation: the window under the fingers grows as they spread.
    const w = Math.min(
      Math.max((r0.w * distance) / gesture.startDistance, limits.value.minW),
      limits.value.maxW
    );
    const ratio = w / r0.w;
    rect.value = clamp({
      x: mid.x - (gesture.startMid.x - r0.x) * ratio,
      y: mid.y - (gesture.startMid.y - r0.y) * ratio,
      w,
      h: w / aspect.value,
    });
  } else if (gesture.type === "move") {
    const p = toImage(event.clientX, event.clientY);
    const r0 = gesture.startRect;
    rect.value = clamp({
      x: r0.x + (p.x - gesture.start.x),
      y: r0.y + (p.y - gesture.start.y),
      w: r0.w,
      h: r0.h,
    });
  } else if (gesture.type === "resize") {
    const p = toImage(event.clientX, event.clientY);
    const { anchor, handle } = gesture;
    const sx = handle.includes("w") ? -1 : 1;
    const sy = handle.includes("n") ? -1 : 1;
    // Project the pointer onto the window's diagonal so the corner follows it.
    const dx = (p.x - anchor.x) * sx;
    const dy = (p.y - anchor.y) * sy;
    const w = (dx + dy * aspect.value) / 2;
    const h = w / aspect.value;
    rect.value = clamp({
      x: sx > 0 ? anchor.x : anchor.x - w,
      y: sy > 0 ? anchor.y : anchor.y - h,
      w,
      h,
    });
  }
  schedulePreview(350);
}

function onPointerUp(event) {
  if (!pointers.has(event.pointerId)) return;
  pointers.delete(event.pointerId);
  if (pointers.size === 1 && gesture?.type === "pinch") {
    // One finger left: continue as a plain move from where it is.
    const [remaining] = [...pointers.entries()];
    startGesture("move", { clientX: remaining[1].x, clientY: remaining[1].y }, null);
    return;
  }
  if (pointers.size === 0) {
    gesture = null;
    gestureActive.value = false;
    activeHandle.value = null;
    refreshView(true);
    schedulePreview(60);
  }
}

let wheelTimer = null;
function onWheel(event) {
  const p = toImage(event.clientX, event.clientY);
  const r = rect.value;
  // Wheel up zooms in (a tighter window). A trackpad pinch arrives as a wheel
  // event with ctrlKey: there the window follows the fingers, like touch.
  const factor = event.ctrlKey ? Math.exp(-event.deltaY * 0.01) : Math.exp(event.deltaY * 0.0015);
  const w = Math.min(Math.max(r.w * factor, limits.value.minW), limits.value.maxW);
  const ratio = w / r.w;
  animate.value = false;
  rect.value = clamp({
    x: p.x - (p.x - r.x) * ratio,
    y: p.y - (p.y - r.y) * ratio,
    w,
    h: w / aspect.value,
  });
  clearTimeout(wheelTimer);
  wheelTimer = setTimeout(() => refreshView(true), 160);
  schedulePreview(200);
}

function onKeyDown(event) {
  const r = rect.value;
  const step = (event.shiftKey ? 0.05 : 0.01) * Math.max(editSize.value.w, editSize.value.h);
  const zoomBy = (factor) => {
    const w = Math.min(Math.max(r.w * factor, limits.value.minW), limits.value.maxW);
    const cx = r.x + r.w / 2;
    const cy = r.y + r.h / 2;
    setRect({ x: cx - w / 2, y: cy - w / aspect.value / 2, w, h: w / aspect.value });
  };
  switch (event.key) {
    case "ArrowLeft":
      setRect({ ...r, x: r.x - step });
      break;
    case "ArrowRight":
      setRect({ ...r, x: r.x + step });
      break;
    case "ArrowUp":
      setRect({ ...r, y: r.y - step });
      break;
    case "ArrowDown":
      setRect({ ...r, y: r.y + step });
      break;
    case "+":
    case "=":
      zoomBy(0.9);
      break;
    case "-":
    case "_":
      zoomBy(1.1);
      break;
    case "r":
      rotate(90);
      break;
    case "R":
      rotate(-90);
      break;
    case "0":
      useCover();
      break;
    default:
      return;
  }
  event.preventDefault();
}

// ---------------------------------------------------------------------------
// "On the frame" preview: small, dithered, throttled
// ---------------------------------------------------------------------------
let previewTimer = null;
let lastPreviewAt = 0;

function schedulePreview(delay = 120) {
  if (previewTimer) return; // already pending
  const wait = gestureActive.value
    ? Math.max(delay, 350 - (performance.now() - lastPreviewAt))
    : delay;
  previewTimer = setTimeout(
    () => {
      previewTimer = null;
      renderPreview();
    },
    Math.max(0, wait)
  );
}

function renderPreview() {
  if (!editCanvas || !props.modelValue) return;
  const scale = PREVIEW_LONG_SIDE / Math.max(props.frame.width, props.frame.height);
  const pw = Math.max(2, Math.round(props.frame.width * scale));
  const ph = Math.max(2, Math.round(props.frame.height * scale));
  const { zoom, panX, panY } = rectToZoomPan(rect.value, pw);
  previewBusy.value = true;
  try {
    const result = processImage(editCanvas, {
      displayWidth: pw,
      displayHeight: ph,
      palette: props.palette,
      params: props.params,
      scaleMode: "custom",
      backgroundColor: background.value,
      zoom,
      panX,
      panY,
      usePerceivedOutput: true,
    });
    for (const target of [previewRef.value, pipRef.value]) {
      if (!target) continue;
      target.width = pw;
      target.height = ph;
      target.getContext("2d").drawImage(result.canvas, 0, 0);
    }
  } catch (error) {
    console.error("Framing preview failed:", error);
  } finally {
    previewBusy.value = false;
    lastPreviewAt = performance.now();
  }
}

// The PiP canvas mounts/unmounts with the breakpoint; repaint when it appears.
watch([previewRef, pipRef], () => {
  if (props.modelValue) schedulePreview(0);
});

const frameLabel = computed(
  () =>
    `${props.frame.width} × ${props.frame.height} · ${frameIsPortrait.value ? "portrait" : "landscape"}`
);
</script>

<template>
  <v-dialog
    :model-value="modelValue"
    :fullscreen="smAndDown"
    :max-width="smAndDown ? undefined : 1100"
    :scrim="'#1f1b17'"
    transition="dialog-bottom-transition"
    @update:model-value="close"
  >
    <v-card class="crop-editor" :class="{ compact: smAndDown }" rounded="lg">
      <div class="ce-head">
        <div>
          <div class="text-subtitle-1 font-weight-medium">Adjust framing</div>
          <div class="text-caption text-medium-emphasis">{{ frameLabel }}</div>
        </div>
        <v-btn icon="mdi-close" variant="text" size="small" aria-label="Close" @click="close" />
      </div>

      <div class="ce-body">
        <!-- Stage: the photo, the frame window, and the gestures -->
        <div
          ref="stageRef"
          class="ce-stage"
          :class="{ animate: animate && !gestureActive, dragging: gestureActive }"
          tabindex="0"
          role="application"
          aria-label="Framing: drag to move, pinch or scroll to zoom, arrow keys to nudge"
          @pointerdown="onPointerDown"
          @pointermove="onPointerMove"
          @pointerup="onPointerUp"
          @pointercancel="onPointerUp"
          @wheel.prevent="onWheel"
          @keydown="onKeyDown"
        >
          <!-- Letterbox background: only visible where the photo does not cover the window -->
          <div class="ce-bars" :style="{ ...boxStyle, background: backgroundHex }" />
          <div ref="imageHostRef" class="ce-image" :style="imageStyle" />
          <div class="ce-box" :style="boxStyle">
            <div class="ce-grid" aria-hidden="true" />
            <div
              v-for="h in ['nw', 'ne', 'sw', 'se']"
              :key="h"
              class="ce-handle"
              :class="[`ce-handle-${h}`, { active: activeHandle === h }]"
              :data-handle="h"
              aria-hidden="true"
            />
            <div
              v-if="enlargementLabel"
              class="ce-chip"
              :class="{ severe: enlargementSevere }"
              :title="'The photo is smaller than the frame here; it will look softer'"
            >
              {{ enlargementLabel }}
            </div>
          </div>

          <!-- Picture-in-picture preview on small screens -->
          <template v-if="smAndDown">
            <button
              v-if="!pipHidden"
              type="button"
              class="ce-pip"
              :class="{ portrait: frameIsPortrait }"
              title="Hide preview"
              @pointerdown.stop
              @click="pipHidden = true"
            >
              <canvas ref="pipRef" />
              <span class="ce-pip-label">On the frame</span>
            </button>
            <v-btn
              v-else
              class="ce-pip-show"
              icon="mdi-eye-outline"
              size="small"
              variant="tonal"
              title="Show preview"
              @pointerdown.stop
              @click="pipHidden = false"
            />
          </template>
        </div>

        <!-- Side panel: live preview and tools -->
        <aside class="ce-panel">
          <div v-if="!smAndDown" class="ce-preview" :class="{ portrait: frameIsPortrait }">
            <canvas ref="previewRef" />
            <div class="ce-preview-label">
              <span>On the frame</span>
              <v-progress-circular v-if="previewBusy" indeterminate size="12" width="2" />
            </div>
          </div>

          <div class="ce-tools">
            <v-btn
              :stacked="smAndDown"
              variant="tonal"
              size="small"
              prepend-icon="mdi-rotate-left"
              title="Rotate left"
              aria-label="Rotate left"
              @click="rotate(-90)"
            >
              <span class="ce-tool-label">{{ smAndDown ? "Rotate" : "Rotate left" }}</span>
            </v-btn>
            <v-btn
              :stacked="smAndDown"
              variant="tonal"
              size="small"
              prepend-icon="mdi-rotate-right"
              title="Rotate right"
              aria-label="Rotate right"
              @click="rotate(90)"
            >
              <span class="ce-tool-label">{{ smAndDown ? "Rotate" : "Rotate right" }}</span>
            </v-btn>
            <v-btn
              :stacked="smAndDown"
              variant="tonal"
              size="small"
              prepend-icon="mdi-flip-horizontal"
              :active="flipH"
              title="Mirror"
              aria-label="Mirror"
              @click="mirror"
            >
              <span class="ce-tool-label">Mirror</span>
            </v-btn>
            <v-btn
              :stacked="smAndDown"
              variant="tonal"
              size="small"
              prepend-icon="mdi-crop-free"
              title="Fill the frame with the photo"
              aria-label="Fill the frame with the photo"
              @click="useCover"
            >
              <span class="ce-tool-label">Fill</span>
            </v-btn>
            <v-btn
              :stacked="smAndDown"
              variant="tonal"
              size="small"
              prepend-icon="mdi-fit-to-screen"
              title="Show the whole photo"
              aria-label="Show the whole photo"
              @click="useFit"
            >
              <span class="ce-tool-label">Fit</span>
            </v-btn>
          </div>

          <v-expand-transition>
            <div v-if="hasBars" class="ce-bg">
              <span class="text-caption text-medium-emphasis">Bars</span>
              <v-btn-toggle
                v-model="background"
                mandatory
                density="compact"
                variant="outlined"
                color="primary"
              >
                <v-btn value="white" size="small">White</v-btn>
                <v-btn value="black" size="small">Black</v-btn>
              </v-btn-toggle>
            </div>
          </v-expand-transition>

          <p v-if="!smAndDown" class="ce-hint text-caption text-medium-emphasis">
            Drag to move. Scroll or pinch to zoom. Double-click to fill again.
          </p>
        </aside>
      </div>

      <div class="ce-actions">
        <v-btn variant="text" @click="close">Cancel</v-btn>
        <v-spacer />
        <v-btn color="primary" prepend-icon="mdi-check" @click="apply">Apply</v-btn>
      </div>
    </v-card>
  </v-dialog>
</template>

<style scoped>
.crop-editor {
  --stage-bg: #1f1b17;
  --mask: rgba(31, 27, 23, 0.66);
  --line: rgba(255, 255, 255, 0.92);
  display: flex;
  flex-direction: column;
  max-height: 92vh;
}
.crop-editor.compact {
  max-height: none;
  height: 100dvh;
  border-radius: 0 !important;
}

.ce-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 12px 8px 8px 20px;
}

.ce-body {
  display: grid;
  grid-template-columns: minmax(0, 1fr) 272px;
  gap: 16px;
  padding: 0 20px;
  min-height: 0;
  flex: 1 1 auto;
}
.compact .ce-body {
  grid-template-columns: minmax(0, 1fr);
  grid-template-rows: minmax(0, 1fr) auto;
  gap: 8px;
  padding: 0 8px;
}

/* ---- Stage --------------------------------------------------------------- */
.ce-stage {
  position: relative;
  overflow: hidden;
  background: var(--stage-bg);
  border-radius: 12px;
  height: clamp(340px, 68vh, 760px);
  touch-action: none;
  user-select: none;
  cursor: grab;
  outline: none;
}
.compact .ce-stage {
  height: auto;
  min-height: 0;
  border-radius: 10px;
}
.ce-stage:focus-visible {
  box-shadow: inset 0 0 0 2px rgb(var(--v-theme-primary));
}
.ce-stage.dragging {
  cursor: grabbing;
}

.ce-image,
.ce-bars,
.ce-box {
  position: absolute;
  top: 0;
  left: 0;
  transform-origin: 0 0;
  will-change: transform;
}
.ce-stage.animate .ce-image,
.ce-stage.animate .ce-bars,
.ce-stage.animate .ce-box {
  transition:
    transform 0.22s cubic-bezier(0.2, 0.7, 0.2, 1),
    width 0.22s cubic-bezier(0.2, 0.7, 0.2, 1),
    height 0.22s cubic-bezier(0.2, 0.7, 0.2, 1);
}
.ce-image :deep(canvas) {
  display: block;
  width: 100%;
  height: 100%;
}

/* The window: everything outside it is dimmed by one large shadow so the
   mask and the window always move as one. */
.ce-box {
  box-shadow:
    0 0 0 1px rgba(0, 0, 0, 0.35),
    0 0 0 9999px var(--mask);
  outline: 1px solid var(--line);
  outline-offset: -1px;
}

.ce-grid {
  position: absolute;
  inset: 0;
  pointer-events: none;
  background-image:
    linear-gradient(to right, rgba(255, 255, 255, 0.28) 1px, transparent 1px),
    linear-gradient(to bottom, rgba(255, 255, 255, 0.28) 1px, transparent 1px);
  background-size: 33.34% 33.34%;
  background-position: -1px -1px;
  opacity: 0.6;
  transition: opacity 0.15s;
}
.ce-stage.dragging .ce-grid {
  opacity: 1;
}

/* Viewfinder brackets: a 44px hit area with an L-shaped mark in the corner. */
.ce-handle {
  position: absolute;
  width: 44px;
  height: 44px;
  cursor: nwse-resize;
}
.ce-handle::before {
  content: "";
  position: absolute;
  width: 22px;
  height: 22px;
  border: 3px solid var(--line);
  border-radius: 2px;
  filter: drop-shadow(0 0 1px rgba(0, 0, 0, 0.5));
  transition: border-color 0.15s;
}
.ce-handle.active::before {
  border-color: rgb(var(--v-theme-primary));
}
.ce-handle-nw {
  top: -22px;
  left: -22px;
}
.ce-handle-nw::before {
  right: 0;
  bottom: 0;
  border-right: 0;
  border-bottom: 0;
  border-top-left-radius: 6px;
}
.ce-handle-ne {
  top: -22px;
  right: -22px;
  cursor: nesw-resize;
}
.ce-handle-ne::before {
  left: 0;
  bottom: 0;
  border-left: 0;
  border-bottom: 0;
  border-top-right-radius: 6px;
}
.ce-handle-sw {
  bottom: -22px;
  left: -22px;
  cursor: nesw-resize;
}
.ce-handle-sw::before {
  right: 0;
  top: 0;
  border-right: 0;
  border-top: 0;
  border-bottom-left-radius: 6px;
}
.ce-handle-se {
  bottom: -22px;
  right: -22px;
}
.ce-handle-se::before {
  left: 0;
  top: 0;
  border-left: 0;
  border-top: 0;
  border-bottom-right-radius: 6px;
}

.ce-chip {
  position: absolute;
  top: 8px;
  left: 8px;
  padding: 2px 8px;
  border-radius: 999px;
  background: rgba(31, 27, 23, 0.78);
  color: #fff;
  font-size: 11px;
  line-height: 18px;
  pointer-events: none;
  white-space: nowrap;
}
.ce-chip.severe {
  background: rgb(var(--v-theme-warning));
}

/* Picture-in-picture preview (small screens) */
.ce-pip {
  position: absolute;
  right: 10px;
  bottom: 10px;
  width: 26%;
  max-width: 132px;
  padding: 0;
  border: 0;
  background: transparent;
  cursor: pointer;
  text-align: left;
}
.ce-pip.portrait {
  width: 22%;
  max-width: 108px;
}
.ce-pip canvas,
.ce-preview canvas {
  display: block;
  width: 100%;
  height: auto;
  border-radius: 6px;
  box-shadow:
    0 0 0 1px rgba(255, 255, 255, 0.85),
    0 4px 14px rgba(0, 0, 0, 0.45);
  image-rendering: auto;
}
.ce-pip-label {
  display: block;
  margin-top: 4px;
  color: rgba(255, 255, 255, 0.82);
  font-size: 11px;
  line-height: 14px;
  text-shadow: 0 1px 2px rgba(0, 0, 0, 0.6);
}
.ce-pip-show {
  position: absolute;
  right: 10px;
  bottom: 10px;
}

/* ---- Panel --------------------------------------------------------------- */
.ce-panel {
  display: flex;
  flex-direction: column;
  gap: 14px;
  min-width: 0;
}
.compact .ce-panel {
  flex-direction: row;
  flex-wrap: wrap;
  align-items: center;
  justify-content: center;
  gap: 8px;
}

.ce-preview {
  align-self: center;
  width: 100%;
  max-width: 236px;
}
.ce-preview.portrait {
  max-width: 180px;
}
.ce-preview canvas {
  box-shadow:
    0 0 0 1px rgba(0, 0, 0, 0.12),
    0 6px 18px rgba(0, 0, 0, 0.18);
}
.ce-preview-label {
  display: flex;
  align-items: center;
  justify-content: center;
  gap: 8px;
  margin-top: 8px;
  font-size: 12px;
  color: rgba(0, 0, 0, 0.6);
}

.ce-tools {
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 8px;
}
.ce-tools .v-btn {
  justify-content: flex-start;
}
.compact .ce-tools {
  display: flex;
  justify-content: center;
  gap: 4px;
}
.compact .ce-tools .v-btn {
  min-width: 60px;
  padding: 0 6px;
}
.compact .ce-tool-label {
  font-size: 11px;
}

.ce-bg {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
}
.compact .ce-bg {
  width: 100%;
  justify-content: center;
}

.ce-hint {
  margin: 0;
  line-height: 1.45;
}

.ce-actions {
  display: flex;
  align-items: center;
  padding: 12px 20px 16px;
}
.compact .ce-actions {
  padding: 8px 12px calc(10px + env(safe-area-inset-bottom));
}

@media (prefers-reduced-motion: reduce) {
  .ce-stage.animate .ce-image,
  .ce-stage.animate .ce-bars,
  .ce-stage.animate .ce-box,
  .ce-grid,
  .ce-handle::before {
    transition: none;
  }
}
</style>
