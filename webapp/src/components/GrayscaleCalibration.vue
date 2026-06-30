<script setup>
import { ref, onMounted, onUnmounted, watch } from "vue";
import { useSettingsStore } from "../stores";

const settingsStore = useSettingsStore();

const saving = ref(false);
const displaying = ref(false);
const snackbar = ref(false);
const snackbarText = ref("");
const snackbarColor = ref("success");

// Local field models. The firmware stores Y as a float32, so a saved value
// round-trips with noise (0.90 -> 0.899999976...); round for display so the
// inputs read cleanly. 3 digits, since black_y is ~0.009.
const blackY = ref(0.009);
const whiteY = ref(0.65);
const gamma = ref(1.42);
const round3 = (v) => Math.round((Number(v) || 0) * 1000) / 1000;

function applyToStore() {
  settingsStore.palette.black_y = round3(blackY.value);
  settingsStore.palette.white_y = round3(whiteY.value);
  settingsStore.palette.gamma = round3(gamma.value);
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
  blackY.value = round3(
    typeof settingsStore.palette.black_y === "number" ? settingsStore.palette.black_y : 0.009
  );
  whiteY.value = round3(
    typeof settingsStore.palette.white_y === "number" ? settingsStore.palette.white_y : 0.65
  );
  gamma.value = round3(
    typeof settingsStore.palette.gamma === "number" ? settingsStore.palette.gamma : 1.42
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

// Show the raw 16-level gray step wedge on the panel (the GC16 analog of the
// Spectra E6 calibration swatches). This is the panel's true per-level response
// to compare the on-screen preview against while tuning the values above.
async function displayCalibration() {
  displaying.value = true;
  try {
    const response = await fetch("/api/calibration/display", { method: "POST" });
    if (response.ok) {
      showSnackbar("Step wedge displayed on the panel", "success");
    } else {
      showSnackbar("Failed to display step wedge", "error");
    }
  } catch (_error) {
    showSnackbar("Failed to display step wedge", "error");
  } finally {
    displaying.value = false;
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
      match what the panel actually shows (WYSIWYG). Use <strong>Show Step Wedge</strong> to display
      all 16 raw gray levels on the panel, then adjust <strong>gamma</strong> until the panel's
      mid-tones match the preview (the ends won't move; gamma only reshapes the middle).
    </p>

    <v-row>
      <v-col cols="12" md="6">
        <v-text-field
          v-model.number="blackY"
          label="Black luminance (Y)"
          type="number"
          :min="0"
          :max="1"
          :step="0.001"
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
          hint="Measured relative luminance of full white (0–1). Default 0.65."
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
          hint="Mid-level shaping; 1.0 = neutral, >1 darkens mid-tones. Default 1.42."
          persistent-hint
        />
      </v-col>
    </v-row>

    <div class="d-flex mt-4" style="gap: 8px">
      <v-btn color="primary" :loading="saving" @click="save">
        <v-icon icon="mdi-content-save" start />
        Save Calibration
      </v-btn>
      <v-btn variant="outlined" :loading="displaying" @click="displayCalibration">
        <v-icon icon="mdi-gradient-horizontal" start />
        Show Step Wedge
      </v-btn>
    </div>

    <v-snackbar v-model="snackbar" :color="snackbarColor" timeout="3000">
      {{ snackbarText }}
    </v-snackbar>
  </div>
</template>

<style scoped></style>
