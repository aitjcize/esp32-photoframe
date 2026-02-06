<script setup>
import { ref, onMounted, computed } from "vue";
import { useAppStore, useSettingsStore } from "../stores";
import ImageProcessing from "./ImageProcessing.vue";

const appStore = useAppStore();
const settingsStore = useSettingsStore();

const fileInput = ref(null);
const uploading = ref(false);
const uploadProgress = ref(0);
const selectedFile = ref(null);
const previewUrl = ref(null);
const showPreview = ref(false);
const processedResult = ref(null);
const sourceCanvas = ref(null);

// Display dimensions
// Display dimensions
// Display dimensions from store
const displayWidth = computed(() => appStore.systemInfo.width);
const displayHeight = computed(() => appStore.systemInfo.height);
const THUMBNAIL_WIDTH = 400;
const THUMBNAIL_HEIGHT = 240;

const canSaveToAlbum = computed(() => {
  return appStore.systemInfo.has_sdcard && appStore.systemInfo.sdcard_inserted;
});

// Image processor library
let imageProcessor = null;

onMounted(async () => {
  imageProcessor = await import("@aitjcize/epaper-image-convert");
});

function triggerFileSelect() {
  fileInput.value?.click();
}

async function onFileSelected(event) {
  const file = event.target.files?.[0];
  if (!file) return;
  await processFile(file);
}

async function processFile(file) {
  selectedFile.value = file;

  // Create preview URL
  previewUrl.value = URL.createObjectURL(file);
  showPreview.value = true;

  // Load image and create source canvas for upload processing
  const img = await loadImage(file);
  sourceCanvas.value = document.createElement("canvas");
  sourceCanvas.value.width = img.width;
  sourceCanvas.value.height = img.height;
  const ctx = sourceCanvas.value.getContext("2d");
  ctx.drawImage(img, 0, 0);

  // Switch to processing tab so user can adjust settings
  settingsStore.activeSettingsTab = "processing";
}

function loadImage(file) {
  return new Promise((resolve, reject) => {
    const img = new Image();
    img.onload = () => resolve(img);
    img.onerror = reject;
    img.src = URL.createObjectURL(file);
  });
}

async function uploadImage(mode = "upload") {
  if (!selectedFile.value || !sourceCanvas.value || !imageProcessor) return;

  uploading.value = true;
  uploadProgress.value = 0;

  try {
    // Get processing parameters
    const params = {
      exposure: settingsStore.params.exposure,
      saturation: settingsStore.params.saturation,
      toneMode: settingsStore.params.toneMode,
      contrast: settingsStore.params.contrast,
      strength: settingsStore.params.strength,
      shadowBoost: settingsStore.params.shadowBoost,
      highlightCompress: settingsStore.params.highlightCompress,
      midpoint: settingsStore.params.midpoint,
      colorMethod: settingsStore.params.colorMethod,
      ditherAlgorithm: settingsStore.params.ditherAlgorithm,
      compressDynamicRange: settingsStore.params.compressDynamicRange,
    };

    // Use native board dimensions for upload
    const targetWidth = displayWidth.value;
    const targetHeight = displayHeight.value;
    const palette = imageProcessor.SPECTRA6;

    // Process image with theoretical palette for device
    const result = imageProcessor.processImage(sourceCanvas.value, {
      displayWidth: targetWidth,
      displayHeight: targetHeight,
      palette,
      params,
      skipRotation: false, // Ensure image is rotated to fit the hardware's native physical resolution
      usePerceivedOutput: false, // Use theoretical palette
    });

    // Convert processed canvas to PNG blob
    const pngBlob = await new Promise((resolve) => {
      result.canvas.toBlob(resolve, "image/png");
    });

    // Generate filename with .png extension
    const originalName = selectedFile.value.name.replace(/\.[^/.]+$/, "");
    const pngFilename = `${originalName}.png`;

    // Generate thumbnail from original canvas (before rotation)
    const thumbCanvas = imageProcessor.generateThumbnail(
      result.originalCanvas || sourceCanvas.value,
      THUMBNAIL_WIDTH,
      THUMBNAIL_HEIGHT
    );
    const thumbnailBlob = await new Promise((resolve) => {
      thumbCanvas.toBlob(resolve, "image/jpeg", 0.85);
    });
    const thumbFilename = `${originalName}.jpg`;

    // Create form data
    const formData = new FormData();
    formData.append("image", pngBlob, pngFilename);
    formData.append("thumbnail", thumbnailBlob, thumbFilename);

    // Determine upload URL based on mode and capability
    // If mode is 'display' or SD card not available, use display-image endpoint
    const isDirectDisplay = mode === "display" || !canSaveToAlbum.value;

    const uploadUrl = isDirectDisplay
      ? "/api/display-image"
      : `/api/upload?album=${encodeURIComponent(appStore.selectedAlbum)}`;

    const response = await fetch(uploadUrl, {
      method: "POST",
      body: formData,
    });

    if (response.ok) {
      if (!isDirectDisplay && canSaveToAlbum.value) {
        await appStore.loadImages(appStore.selectedAlbum);
      }

      // Only reset if we are uploading or if we are in no-sdcard mode
      // If we are in display mode with sdcard, keep the UI open for adjustments
      if (!(canSaveToAlbum.value && mode === "display")) {
        resetUpload();
      }
    }
  } catch (error) {
    console.error("Upload failed:", error);
  } finally {
    uploading.value = false;
  }
}

function resetUpload() {
  selectedFile.value = null;
  previewUrl.value = null;
  showPreview.value = false;
  sourceCanvas.value = null;
  if (fileInput.value) {
    fileInput.value.value = "";
  }
  // Switch back to general tab after upload/cancel
  settingsStore.activeSettingsTab = "general";
}

// AI Generation Logic
const showAiDialog = ref(false);
const aiPrompt = ref("");
const aiModel = ref("gpt-image-1.5");
const generatingAi = ref(false);

const aiModelOptions = [
  { title: "GPT Image 1.5", value: "gpt-image-1.5" },
  { title: "GPT Image 1", value: "gpt-image-1" },
  { title: "GPT Image 1 Mini", value: "gpt-image-1-mini" },
];

const aiProvider = ref(0);
const aiProviderOptions = [{ title: "OpenAI", value: 0 }];

const snackbar = ref(false);
const snackbarText = ref("");
const snackbarColor = ref("info");

function showMessage(text, color = "info") {
  snackbarText.value = text;
  snackbarColor.value = color;
  snackbar.value = true;
}

function openAiDialog() {
  const provider = settingsStore.deviceSettings.aiSettings.aiProvider;
  const openaiKey = settingsStore.deviceSettings.aiSettings.openaiApiKey;
  const googleKey = settingsStore.deviceSettings.aiSettings.googleApiKey;

  if (provider === 0 && !openaiKey) {
    showMessage("Please configure OpenAI API Key in Settings > AI Generation first.", "error");
    settingsStore.activeSettingsTab = "ai";
    return;
  }
  if (provider === 1 && !googleKey) {
    showMessage(
      "Please configure Google Gemini API Key in Settings > AI Generation first.",
      "error"
    );
    settingsStore.activeSettingsTab = "ai";
    return;
  }

  aiPrompt.value = "";
  aiProvider.value = settingsStore.deviceSettings.aiSettings.aiProvider;
  aiModel.value = settingsStore.deviceSettings.aiSettings.aiModel || "gpt-image-1.5";
  showAiDialog.value = true;
}

async function generateAiImage() {
  generatingAi.value = true;
  try {
    const provider = aiProvider.value;
    const apiKey =
      provider === 0
        ? settingsStore.deviceSettings.aiSettings.openaiApiKey
        : settingsStore.deviceSettings.aiSettings.googleApiKey;

    if (provider !== 0) {
      throw new Error("Frontend generation only supported for OpenAI currently.");
    }

    const isPortrait = settingsStore.deviceSettings.displayOrientation === "portrait";
    const size = isPortrait ? "1024x1536" : "1536x1024";

    const response = await fetch("https://api.openai.com/v1/images/generations", {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
        Authorization: `Bearer ${apiKey}`,
      },
      body: JSON.stringify({
        model: aiModel.value,
        prompt: aiPrompt.value,
        n: 1,
        size: isPortrait ? "1024x1536" : "1536x1024",
        quality: "high",
        output_format: "jpeg",
        output_compression: 90,
      }),
    });

    if (!response.ok) {
      const errorText = await response.text();
      throw new Error(`API Error: ${response.status} - ${errorText}`);
    }

    const data = await response.json();
    if (!data.data?.[0]?.b64_json) {
      throw new Error("Invalid response from AI API: missing image data");
    }

    const src = `data:image/jpeg;base64,${data.data[0].b64_json}`;

    const res = await fetch(src);
    const blob = await res.blob();
    const file = new File([blob], "ai-generated.jpg", { type: "image/jpeg" });

    showAiDialog.value = false;
    await processFile(file);
    showMessage("AI image generated successfully!", "success");
  } catch (error) {
    showMessage(`Generation failed: ${error.message}`, "error");
  } finally {
    generatingAi.value = false;
  }
}
</script>

<template>
  <v-card class="mt-4">
    <v-card-title class="d-flex align-center">
      <v-icon icon="mdi-upload" class="mr-2" />
      Upload Image
    </v-card-title>

    <v-card-text>
      <!-- Hidden file input -->
      <input
        ref="fileInput"
        type="file"
        accept=".jpg,.jpeg,.png,.heic,.heif,.webp,.gif,.bmp"
        style="display: none"
        @change="onFileSelected"
      />

      <!-- Upload Area -->
      <v-sheet
        v-if="!showPreview"
        class="upload-zone d-flex flex-column align-center justify-center pa-8"
        rounded
        border
        @click="triggerFileSelect"
        @dragover.prevent
        @drop.prevent="onFileSelected({ target: { files: $event.dataTransfer.files } })"
      >
        <v-icon icon="mdi-cloud-upload" size="64" color="grey" />
        <p class="text-h6 mt-4">Click or drag image to upload</p>
        <p class="text-body-2 text-grey">Supports: JPG, PNG, HEIC, WebP, GIF, BMP</p>
        <div class="my-3 d-flex align-center" style="width: 100%">
          <v-divider />
          <span class="mx-2 text-grey text-caption">OR</span>
          <v-divider />
        </div>
        <v-btn color="primary" variant="tonal" @click.stop="openAiDialog">
          <v-icon icon="mdi-magic-staff" start />
          Generate with AI
        </v-btn>
      </v-sheet>

      <!-- Preview Area with Processing -->
      <div v-else>
        <ImageProcessing
          :image-file="selectedFile"
          :params="settingsStore.params"
          :palette="settingsStore.palette"
          @processed="processedResult = $event"
        />
      </div>
    </v-card-text>

    <v-card-actions v-if="showPreview" class="px-4 pb-4">
      <v-btn variant="text" @click="resetUpload"> Cancel </v-btn>
      <v-spacer />
      <v-select
        v-if="canSaveToAlbum"
        v-model="appStore.selectedAlbum"
        :items="appStore.sortedAlbums.map((a) => a.name)"
        label="Album"
        variant="outlined"
        density="compact"
        hide-details
        style="max-width: 200px"
        class="mr-2"
      />
      <v-btn
        v-if="canSaveToAlbum"
        color="secondary"
        class="mr-2"
        :loading="uploading"
        @click="uploadImage('display')"
      >
        <v-icon icon="mdi-monitor" start />
        Display
      </v-btn>
      <v-btn color="primary" :loading="uploading" @click="uploadImage('upload')">
        <v-icon :icon="canSaveToAlbum ? 'mdi-upload' : 'mdi-monitor'" start />
        {{ canSaveToAlbum ? "Upload" : "Display" }}
      </v-btn>
    </v-card-actions>

    <!-- Upload Progress -->
    <v-progress-linear v-if="uploading" :model-value="uploadProgress" color="primary" height="4" />

    <!-- AI Input Dialog -->
    <v-dialog v-model="showAiDialog" max-width="500">
      <v-card>
        <v-card-title>Generate Image</v-card-title>
        <v-card-text>
          <v-select
            v-model="aiProvider"
            :items="aiProviderOptions"
            item-title="title"
            item-value="value"
            label="Provider"
            variant="outlined"
            class="mb-4"
          />
          <v-select
            v-model="aiModel"
            :items="aiModelOptions"
            item-title="title"
            item-value="value"
            label="Model"
            variant="outlined"
            class="mb-4"
          />
          <v-textarea
            v-model="aiPrompt"
            label="Prompt"
            variant="outlined"
            rows="3"
            auto-grow
            hint="Describe the image you want to generate"
            persistent-hint
          />
        </v-card-text>
        <v-card-actions>
          <v-spacer />
          <v-btn variant="text" @click="showAiDialog = false">Cancel</v-btn>
          <v-btn color="primary" :loading="generatingAi" @click="generateAiImage"> Generate </v-btn>
        </v-card-actions>
      </v-card>
    </v-dialog>
    <!-- Snackbar for notifications -->
    <v-snackbar v-model="snackbar" :color="snackbarColor" :timeout="4000">
      {{ snackbarText }}
      <template #actions>
        <v-btn variant="text" @click="snackbar = false"> Close </v-btn>
      </template>
    </v-snackbar>
  </v-card>
</template>

<style scoped>
.upload-zone {
  cursor: pointer;
  min-height: 200px;
  transition: background-color 0.2s;
}
.upload-zone:hover {
  background-color: rgba(0, 0, 0, 0.04);
}
</style>
