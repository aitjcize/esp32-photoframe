<script setup>
import { computed } from "vue";
import { getDitherOptions, getPresetOptions } from "@aitjcize/epaper-image-convert";
import { useAppStore } from "../stores";

const appStore = useAppStore();

const props = defineProps({
  params: {
    type: Object,
    required: true,
  },
  preset: {
    type: String,
    default: "custom",
  },
});

const emit = defineEmits(["update:params", "update:preset", "preset-change"]);

const presetOptions = [
  ...getPresetOptions(),
  { value: "custom", title: "Custom", description: "Manually adjusted parameters" },
];

const ditherOptions = getDitherOptions();

const toneModeOptions = [
  { title: "Contrast", value: "contrast" },
  { title: "S-Curve", value: "scurve" },
];

const colorMethodOptions = [
  { title: "RGB", value: "rgb" },
  { title: "LAB", value: "lab" },
];

const isScurveMode = computed(() => props.params.toneMode === "scurve");

function onPresetChange(value) {
  emit("update:preset", value);
  emit("preset-change", value);
}

function updateParam(key, value) {
  emit("update:params", { ...props.params, [key]: value });
}
</script>

<template>
  <div class="processing-controls">
    <!-- Preset Selection -->
    <v-row>
      <v-col cols="12">
        <v-card variant="outlined" class="mb-6">
          <v-card-subtitle class="pt-3"> Processing Preset </v-card-subtitle>
          <v-card-text>
            <v-btn-toggle
              :model-value="preset"
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
          :model-value="params.ditherAlgorithm"
          :items="ditherOptions"
          item-title="title"
          item-value="value"
          label="Dithering Algorithm"
          variant="outlined"
          density="compact"
          @update:model-value="updateParam('ditherAlgorithm', $event)"
        />
      </v-col>

      <v-col cols="12" md="4">
        <v-select
          :model-value="params.colorMethod"
          :items="colorMethodOptions"
          item-title="title"
          item-value="value"
          label="Color Matching"
          variant="outlined"
          density="compact"
          @update:model-value="updateParam('colorMethod', $event)"
        />
      </v-col>
    </v-row>

    <v-row>
      <!-- Exposure -->
      <v-col cols="12" md="4">
        <v-slider
          :model-value="params.exposure"
          :min="0.5"
          :max="2.0"
          :step="0.01"
          label="Exposure"
          thumb-label
          color="primary"
          @update:model-value="updateParam('exposure', $event)"
        >
          <template #append>
            <span class="text-body-2">{{ params.exposure.toFixed(2) }}</span>
          </template>
        </v-slider>
      </v-col>

      <!-- Saturation (hidden on grayscale panels) -->
      <v-col v-if="!appStore.isGrayscale" cols="12" md="4">
        <v-slider
          :model-value="params.saturation"
          :min="0.5"
          :max="2.0"
          :step="0.01"
          label="Saturation"
          thumb-label
          color="primary"
          @update:model-value="updateParam('saturation', $event)"
        >
          <template #append>
            <span class="text-body-2">{{ params.saturation.toFixed(2) }}</span>
          </template>
        </v-slider>
      </v-col>

      <!-- Compress Dynamic Range -->
      <v-col cols="12" md="4">
        <v-checkbox
          :model-value="params.compressDynamicRange"
          label="Compress Dynamic Range"
          hint="Map brightness to display's actual white point"
          persistent-hint
          color="primary"
          @update:model-value="updateParam('compressDynamicRange', $event)"
        />
      </v-col>
    </v-row>

    <!-- Tone Mode -->
    <v-row>
      <v-col cols="12" md="4">
        <v-select
          :model-value="params.toneMode"
          :items="toneModeOptions"
          item-title="title"
          item-value="value"
          label="Tone Mapping"
          variant="outlined"
          density="compact"
          @update:model-value="updateParam('toneMode', $event)"
        />
      </v-col>

      <!-- Contrast (for contrast mode) -->
      <v-col v-if="!isScurveMode" cols="12" md="4">
        <v-slider
          :model-value="params.contrast"
          :min="0.5"
          :max="2.0"
          :step="0.01"
          label="Contrast"
          thumb-label
          color="primary"
          @update:model-value="updateParam('contrast', $event)"
        >
          <template #append>
            <span class="text-body-2">{{ params.contrast.toFixed(2) }}</span>
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
                :model-value="params.strength"
                :min="0"
                :max="1"
                :step="0.01"
                label="Strength"
                thumb-label
                color="primary"
                @update:model-value="updateParam('strength', $event)"
              >
                <template #append>
                  <span class="text-body-2">{{ params.strength.toFixed(2) }}</span>
                </template>
              </v-slider>
            </v-col>

            <v-col cols="12" md="6">
              <v-slider
                :model-value="params.shadowBoost"
                :min="0"
                :max="1"
                :step="0.01"
                label="Shadow Boost"
                thumb-label
                color="primary"
                @update:model-value="updateParam('shadowBoost', $event)"
              >
                <template #append>
                  <span class="text-body-2">{{ params.shadowBoost.toFixed(2) }}</span>
                </template>
              </v-slider>
            </v-col>

            <v-col cols="12" md="6">
              <v-slider
                :model-value="params.highlightCompress"
                :min="0.5"
                :max="5"
                :step="0.01"
                label="Highlight Compress"
                thumb-label
                color="primary"
                @update:model-value="updateParam('highlightCompress', $event)"
              >
                <template #append>
                  <span class="text-body-2">{{ params.highlightCompress.toFixed(2) }}</span>
                </template>
              </v-slider>
            </v-col>

            <v-col cols="12" md="6">
              <v-slider
                :model-value="params.midpoint"
                :min="0.3"
                :max="0.7"
                :step="0.01"
                label="Midpoint"
                thumb-label
                color="primary"
                @update:model-value="updateParam('midpoint', $event)"
              >
                <template #append>
                  <span class="text-body-2">{{ params.midpoint.toFixed(2) }}</span>
                </template>
              </v-slider>
            </v-col>
          </v-row>
        </v-card-text>
      </v-card>
    </v-expand-transition>
  </div>
</template>
