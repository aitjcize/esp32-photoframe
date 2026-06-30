<script setup>
import { ref, onMounted, onUnmounted, watch } from "vue";
import { useSettingsStore } from "../stores";

const settingsStore = useSettingsStore();

const saving = ref(false);
const snackbar = ref(false);
const snackbarText = ref("");
const snackbarColor = ref("success");

// Local field models. The firmware stores Y as a float32, so a saved 0.90
// round-trips as 0.899999976...; round for display so the inputs read cleanly.
const blackY = ref(0);
const whiteY = ref(0.9);
const gamma = ref(1);
const round2 = (v) => Math.round((Number(v) || 0) * 100) / 100;

function applyToStore() {
  settingsStore.palette.black_y = round2(blackY.value);
  settingsStore.palette.white_y = round2(whiteY.value);
  settingsStore.palette.gamma = round2(gamma.value);
}

// Push edits into the store (which drives the live preview + tone curve) only
// after the user pauses, so the panel re-renders once instead of per keystroke.
let writeTimer = null;
watch([blackY, whiteY, gamma], () => {
  if (writeTimer) clearTimeout(writeTimer);
  writeTimer = setTimeout(applyToStore, 500);
});

onMounted(async () => {
  await settingsStore.loadPalette();
  blackY.value = round2(
    typeof settingsStore.palette.black_y === "number" ? settingsStore.palette.black_y : 0
  );
  whiteY.value = round2(
    typeof settingsStore.palette.white_y === "number" ? settingsStore.palette.white_y : 0.9
  );
  gamma.value = round2(
    typeof settingsStore.palette.gamma === "number" ? settingsStore.palette.gamma : 1
  );
});

onUnmounted(() => {
  if (writeTimer) clearTimeout(writeTimer);
});

function showSnackbar(text, color) {
  snackbarText.value = text;
  snackbarColor.value = color;
  snackbar.value = true;
}

async function save() {
  // Flush any pending debounced edit before saving.
  if (writeTimer) clearTimeout(writeTimer);
  applyToStore();
  saving.value = true;
  try {
    const ok = await settingsStore.savePalette();
    if (ok) {
      showSnackbar("Calibration saved successfully", "success");
    } else {
      showSnackbar("Failed to save calibration", "error");
    }
  } catch (_error) {
    showSnackbar("Failed to save calibration", "error");
  } finally {
    saving.value = false;
  }
}
</script>

<template>
  <div>
    <v-alert type="info" variant="tonal" class="mb-4">
      Calibrate the measured luminance of your grayscale (GC16) panel. These two values are the
      relative luminance (Y, 0&ndash;1) of full black and full white as displayed by your specific
      panel. They drive the grayscale dithering and dynamic-range compression.
    </v-alert>

    <p class="text-body-2 mb-4">
      Keeping <strong>black luminance at 0</strong> renders shadows as pure black for a punchier,
      higher-contrast result. Raising it toward your panel's real black makes the on-screen preview
      match what the panel actually shows (WYSIWYG). You can measure these by displaying a
      full-black image, then a full-white image, and metering the panel's luminance.
    </p>

    <v-row>
      <v-col cols="12" md="6">
        <v-text-field
          v-model.number="blackY"
          label="Black luminance (Y)"
          type="number"
          :min="0"
          :max="1"
          :step="0.01"
          variant="outlined"
          hint="Measured relative luminance of full black (0–1). 0 keeps shadows pure black."
          persistent-hint
        />
      </v-col>
      <v-col cols="12" md="6">
        <v-text-field
          v-model.number="whiteY"
          label="White luminance (Y)"
          type="number"
          :min="0"
          :max="1"
          :step="0.01"
          variant="outlined"
          hint="Measured relative luminance of full white (0–1). Default 0.90."
          persistent-hint
        />
      </v-col>
      <v-col cols="12" md="6">
        <v-text-field
          v-model.number="gamma"
          label="Mid-tone gamma"
          type="number"
          :min="0.2"
          :max="4"
          :step="0.05"
          variant="outlined"
          hint="Mid-level shaping; 1.0 = neutral, >1 darkens mid-tones. Default 1.0."
          persistent-hint
        />
      </v-col>
    </v-row>

    <div class="d-flex mt-4" style="gap: 8px">
      <v-btn color="primary" :loading="saving" @click="save">
        <v-icon icon="mdi-content-save" start />
        Save Calibration
      </v-btn>
    </div>

    <v-snackbar v-model="snackbar" :color="snackbarColor" timeout="3000">
      {{ snackbarText }}
    </v-snackbar>
  </div>
</template>

<style scoped></style>
