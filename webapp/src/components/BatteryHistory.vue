<script setup>
import { ref, computed, onMounted } from "vue";

const API_BASE = "";

const loading = ref(true);
const entries = ref([]); // [{ t: unixSeconds, p: 0-100, c: boolean }]
const daysRemaining = ref(null);

async function loadHistory() {
  loading.value = true;
  try {
    const response = await fetch(`${API_BASE}/api/battery-history`);
    if (!response.ok || response.headers.get("content-type")?.includes("text/html")) {
      return;
    }
    const data = await response.json();
    entries.value = data.entries || [];
    daysRemaining.value = data.days_remaining ?? null;
  } catch (_error) {
    console.log("Battery history not available (standalone mode)");
  } finally {
    loading.value = false;
  }
}

onMounted(loadHistory);

const estimateText = computed(() => {
  if (daysRemaining.value === null) {
    return "Not enough history yet to estimate";
  }
  if (daysRemaining.value <= 0) {
    return "Already at or below the 20% target";
  }
  return `~${daysRemaining.value.toFixed(1)} days until 20%`;
});

// --- Chart geometry (plain SVG, no charting library) ---
const CHART_WIDTH = 800;
const CHART_HEIGHT = 320;
const PAD = { top: 20, right: 24, bottom: 36, left: 44 };
const plotWidth = CHART_WIDTH - PAD.left - PAD.right;
const plotHeight = CHART_HEIGHT - PAD.top - PAD.bottom;

const timeRange = computed(() => {
  if (entries.value.length === 0) return [0, 1];
  const times = entries.value.map((e) => e.t);
  const min = Math.min(...times);
  const max = Math.max(...times);
  return [min, max === min ? min + 1 : max];
});

function xForTime(t) {
  const [min, max] = timeRange.value;
  return PAD.left + ((t - min) / (max - min)) * plotWidth;
}

function yForPercent(p) {
  return PAD.top + plotHeight - (Math.max(0, Math.min(100, p)) / 100) * plotHeight;
}

const linePoints = computed(() =>
  entries.value.map((e) => `${xForTime(e.t).toFixed(1)},${yForPercent(e.p).toFixed(1)}`).join(" ")
);

const targetLineY = computed(() => yForPercent(20));

const yTicks = [0, 20, 40, 60, 80, 100];

// A handful of evenly-spaced date labels along the x-axis (avoids clutter
// with potentially hundreds of points over a long history).
const xTicks = computed(() => {
  if (entries.value.length === 0) return [];
  const [min, max] = timeRange.value;
  const count = 5;
  const ticks = [];
  for (let i = 0; i < count; i++) {
    const t = min + ((max - min) * i) / (count - 1);
    ticks.push({
      x: xForTime(t),
      label: new Date(t * 1000).toLocaleDateString(undefined, { month: "short", day: "numeric" }),
    });
  }
  return ticks;
});

function pointTitle(e) {
  const date = new Date(e.t * 1000).toLocaleString();
  return `${date}\n${e.p}%${e.c ? " (charging)" : ""}`;
}
</script>

<template>
  <v-card>
    <v-card-title class="d-flex align-center">
      <v-icon icon="mdi-battery-clock-outline" class="mr-2" />
      Battery History
    </v-card-title>

    <v-card-text>
      <div v-if="loading" class="d-flex justify-center align-center py-12">
        <v-progress-circular indeterminate color="primary" />
      </div>

      <v-alert v-else-if="entries.length === 0" type="info" variant="tonal">
        No battery history recorded yet. A reading is saved once per displayed image (requires a
        battery to be present and persistent storage to be mounted).
      </v-alert>

      <template v-else>
        <div class="d-flex align-center flex-wrap ga-4 mb-4">
          <v-chip color="primary" variant="tonal" size="large">
            <v-icon icon="mdi-battery-arrow-down-outline" start />
            {{ estimateText }}
          </v-chip>
          <span class="text-caption text-medium-emphasis">
            {{ entries.length }} reading{{ entries.length === 1 ? "" : "s" }} since the last full
            charge or reset
          </span>
        </div>

        <svg
          :viewBox="`0 0 ${CHART_WIDTH} ${CHART_HEIGHT}`"
          preserveAspectRatio="xMidYMid meet"
          style="width: 100%; height: auto; max-height: 360px"
        >
          <!-- Y gridlines + labels -->
          <g v-for="tick in yTicks" :key="'y' + tick">
            <line
              :x1="PAD.left"
              :x2="CHART_WIDTH - PAD.right"
              :y1="yForPercent(tick)"
              :y2="yForPercent(tick)"
              stroke="currentColor"
              stroke-opacity="0.12"
            />
            <text
              :x="PAD.left - 8"
              :y="yForPercent(tick) + 4"
              text-anchor="end"
              font-size="11"
              fill="currentColor"
              fill-opacity="0.6"
            >
              {{ tick }}%
            </text>
          </g>

          <!-- 20% target reference line -->
          <line
            :x1="PAD.left"
            :x2="CHART_WIDTH - PAD.right"
            :y1="targetLineY"
            :y2="targetLineY"
            stroke="#e53935"
            stroke-width="1.5"
            stroke-dasharray="6,4"
            stroke-opacity="0.7"
          />

          <!-- X labels -->
          <text
            v-for="tick in xTicks"
            :key="tick.label + tick.x"
            :x="tick.x"
            :y="CHART_HEIGHT - PAD.bottom + 20"
            text-anchor="middle"
            font-size="11"
            fill="currentColor"
            fill-opacity="0.6"
          >
            {{ tick.label }}
          </text>

          <!-- The battery-level line -->
          <polyline :points="linePoints" fill="none" stroke="#1976d2" stroke-width="2" />

          <!-- Points, colored by charging state -->
          <circle
            v-for="(e, i) in entries"
            :key="i"
            :cx="xForTime(e.t)"
            :cy="yForPercent(e.p)"
            :r="e.c ? 3.5 : 2.5"
            :fill="e.c ? '#fb8c00' : '#1976d2'"
          >
            <title>{{ pointTitle(e) }}</title>
          </circle>
        </svg>

        <div class="d-flex align-center ga-4 mt-2">
          <div class="d-flex align-center ga-1">
            <span class="legend-dot" style="background: #1976d2"></span>
            <span class="text-caption text-medium-emphasis">On battery</span>
          </div>
          <div class="d-flex align-center ga-1">
            <span class="legend-dot" style="background: #fb8c00"></span>
            <span class="text-caption text-medium-emphasis">Charging / USB connected</span>
          </div>
          <div class="d-flex align-center ga-1">
            <span class="legend-line"></span>
            <span class="text-caption text-medium-emphasis">20% target</span>
          </div>
        </div>

        <div class="text-caption text-medium-emphasis mt-4">
          One reading is recorded after each image change. The history (and the estimate above)
          resets automatically once the battery reaches 95% or after 180 days, whichever comes
          first.
        </div>
      </template>
    </v-card-text>
  </v-card>
</template>

<style scoped>
.legend-dot {
  width: 10px;
  height: 10px;
  border-radius: 50%;
  display: inline-block;
}
.legend-line {
  width: 16px;
  height: 0;
  border-top: 1.5px dashed #e53935;
  display: inline-block;
}
</style>
