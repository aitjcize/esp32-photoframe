<script setup>
import { ref, onMounted } from "vue";
import { useSettingsStore } from "../stores";

const settingsStore = useSettingsStore();

const saving = ref(false);
const snackbar = ref(false);
const snackbarText = ref("");
const snackbarColor = ref("success");

onMounted(async () => {
  await settingsStore.loadPalette();
  // Guard against missing keys (e.g. standalone mode or a color-palette payload):
  // ensure both fields exist with sensible GC16 defaults so the inputs bind cleanly.
  if (typeof settingsStore.palette.black_y !== "number") {
    settingsStore.palette.black_y = 0;
  }
  if (typeof settingsStore.palette.white_y !== "number") {
    settingsStore.palette.white_y = 0.9;
  }
});

function showSnackbar(text, color) {
  snackbarText.value = text;
  snackbarColor.value = color;
  snackbar.value = true;
}

async function save() {
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
          v-model.number="settingsStore.palette.black_y"
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
          v-model.number="settingsStore.palette.white_y"
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
