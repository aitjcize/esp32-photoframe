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
// Display dimensions from store
const displayWidth = computed(() => appStore.systemInfo.width);
const displayHeight = computed(() => appStore.systemInfo.height);
const THUMBNAIL_WIDTH = 400;
const THUMBNAIL_HEIGHT = 240;

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

async function uploadImage() {
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

    // Send album as URL query parameter
    const hasSdCard = appStore.systemInfo.has_sdcard;
    const uploadUrl = hasSdCard
      ? `/api/upload?album=${encodeURIComponent(appStore.selectedAlbum)}`
      : "/api/display-image";

    const response = await fetch(uploadUrl, {
      method: "POST",
      body: formData,
    });

    if (response.ok) {
      if (hasSdCard) {
        await appStore.loadImages(appStore.selectedAlbum);
      }
      resetUpload();
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
        v-if="appStore.systemInfo.has_sdcard"
        v-model="appStore.selectedAlbum"
        :items="appStore.sortedAlbums.map((a) => a.name)"
        label="Album"
        variant="outlined"
        density="compact"
        hide-details
        style="max-width: 200px"
        class="mr-2"
      />
      <v-btn color="primary" :loading="uploading" @click="uploadImage">
        <v-icon :icon="appStore.systemInfo.has_sdcard ? 'mdi-upload' : 'mdi-monitor'" start />
        {{ appStore.systemInfo.has_sdcard ? "Upload" : "Display" }}
      </v-btn>
    </v-card-actions>

    <!-- Upload Progress -->
    <v-progress-linear v-if="uploading" :model-value="uploadProgress" color="primary" height="4" />
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
