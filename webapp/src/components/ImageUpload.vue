<script setup>
import { ref, onMounted, computed, watch } from "vue";
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
const imageProcessingRef = ref(null);

// Display dimensions
// Display dimensions
// Display dimensions from store
const displayWidth = computed(() => appStore.systemInfo.width);
const displayHeight = computed(() => appStore.systemInfo.height);
const THUMBNAIL_MAX_DIM = 400;

const canSaveToAlbum = computed(() => {
  return appStore.systemInfo.sdcard_inserted || appStore.systemInfo.has_flash_storage;
});

const formatStorageBytes = (bytes) => {
  if (!bytes) return "0 MB";
  return (bytes / 1024 / 1024).toFixed(1) + " MB";
};
const storageUsedMBString = computed(() => formatStorageBytes(appStore.systemInfo.storage_used));
const storageTotalMBString = computed(() => formatStorageBytes(appStore.systemInfo.storage_total));
const storageUsedPercent = computed(() => {
  const total = appStore.systemInfo.storage_total || 1;
  const used = appStore.systemInfo.storage_used || 0;
  return Math.round((used / total) * 100);
});
const storageColor = computed(() => {
  const pct = storageUsedPercent.value;
  if (pct > 90) return "error";
  if (pct > 75) return "warning";
  return "success";
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

    // Always use native panel dimensions for processing
    const targetWidth = displayWidth.value;
    const targetHeight = displayHeight.value;
    const orientation = settingsStore.deviceSettings.displayOrientation;
    const palette = imageProcessor.SPECTRA6;

    // Get scale mode and params from the preview component
    // Vue auto-unwraps refs from defineExpose, so no .value needed
    const scaleMode = imageProcessingRef.value?.scaleMode || "cover";
    const uploadParams = imageProcessingRef.value?.getUploadParams() || {};

    // Process image with theoretical palette for device at native dimensions.
    // The library handles rotation, scaling (cover/fit/custom), and clean
    // background replacement after dithering.
    const result = imageProcessor.processImage(sourceCanvas.value, {
      displayWidth: targetWidth,
      displayHeight: targetHeight,
      palette,
      params,
      orientation,
      scaleMode,
      backgroundColor: uploadParams.backgroundColorName || "white",
      zoom: uploadParams.zoom,
      panX: uploadParams.panX,
      panY: uploadParams.panY,
      usePerceivedOutput: false, // Use theoretical palette
    });

    // Convert dithered canvas to gzip-compressed 4-bit EPD format using the library
    const compressedBuffer = await imageProcessor.createEPDGZ(result.canvas);
    const rawBlob = new Blob([compressedBuffer], { type: "application/gzip" });

    // Generate filename with .epdgz extension
    const originalName = selectedFile.value.name.replace(/\.[^/.]+$/, "");
    const rawFilename = `${originalName}.epdgz`;

    // Generate thumbnail from original source (clean, unprocessed)
    const thumbCanvas = imageProcessor.generateThumbnail(sourceCanvas.value, THUMBNAIL_MAX_DIM);
    const thumbnailBlob = await new Promise((resolve) => {
      thumbCanvas.toBlob(resolve, "image/jpeg", 0.85);
    });
    const thumbFilename = `${originalName}.jpg`;

    // Create form data
    const formData = new FormData();
    formData.append("image", rawBlob, rawFilename);
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
      // Reload system info to update storage numbers whether in display or album mode
      await appStore.loadSystemInfo();

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

const aiModelOptions = computed(() => {
  if (aiProvider.value === 0) {
    return [
      { title: "GPT Image 1.5", value: "gpt-image-1.5" },
      { title: "GPT Image 1", value: "gpt-image-1" },
      { title: "GPT Image 1 Mini", value: "gpt-image-1-mini" },
      { title: "DALL-E 3", value: "dall-e-3" },
      { title: "DALL-E 2", value: "dall-e-2" },
    ];
  } else {
    return [
      { title: "Gemini 2.5 Flash Image", value: "gemini-2.5-flash-image" },
      { title: "Gemini 3 Pro Image", value: "gemini-3-pro-image-preview" },
    ];
  }
});

const aiProvider = ref(0);
const aiProviderOptions = computed(() => {
  const options = [];
  if (settingsStore.deviceSettings.aiCredentials.openaiApiKey) {
    options.push({ title: "OpenAI", value: 0 });
  }
  if (settingsStore.deviceSettings.aiCredentials.googleApiKey) {
    options.push({ title: "Google Gemini", value: 1 });
  }
  return options;
});

// Reset model to first option when provider changes
watch(aiProvider, (newProvider) => {
  aiModel.value = newProvider === 0 ? "gpt-image-1.5" : "gemini-2.5-flash-image";
});

const showFormatConfirm = ref(false);
const formatting = ref(false);

async function formatStorage() {
  formatting.value = true;
  try {
    const response = await fetch("/api/format-storage", { method: "POST" });
    if (response.ok) {
      showMessage("Storage formatted successfully", "success");
      await appStore.loadSystemInfo();
      await appStore.loadImages(appStore.selectedAlbum);
    } else {
      showMessage("Failed to format storage", "error");
    }
  } catch (error) {
    showMessage(`Format failed: ${error.message}`, "error");
  } finally {
    formatting.value = false;
    showFormatConfirm.value = false;
  }
}

const snackbar = ref(false);
const snackbarText = ref("");
const snackbarColor = ref("info");

function showMessage(text, color = "info") {
  snackbarText.value = text;
  snackbarColor.value = color;
  snackbar.value = true;
}

function openAiDialog() {
  const openaiKey = settingsStore.deviceSettings.aiCredentials.openaiApiKey;
  const googleKey = settingsStore.deviceSettings.aiCredentials.googleApiKey;

  if (!openaiKey && !googleKey) {
    showMessage("Please configure an API Key in Settings > AI Generation first.", "error");
    settingsStore.activeSettingsTab = "ai";
    return;
  }

  aiPrompt.value = "";
  // Default to OpenAI if key exists, otherwise Google
  aiProvider.value = openaiKey ? 0 : 1;
  aiModel.value = aiProvider.value === 0 ? "gpt-image-1.5" : "gemini-2.5-flash-image";
  showAiDialog.value = true;
}

async function generateAiImage() {
  generatingAi.value = true;
  try {
    const provider = aiProvider.value;
    const apiKey =
      provider === 0
        ? settingsStore.deviceSettings.aiCredentials.openaiApiKey
        : settingsStore.deviceSettings.aiCredentials.googleApiKey;

    const isPortrait = settingsStore.deviceSettings.displayOrientation === "portrait";
    let src = null;

    if (provider === 0) {
      // OpenAI
      const isDalle3 = aiModel.value.includes("dall-e-3");
      const isDalle2 = aiModel.value.includes("dall-e-2");
      let size = "1024x1024";

      if (isDalle3) {
        size = isPortrait ? "1024x1792" : "1792x1024";
      } else if (isDalle2) {
        size = "1024x1024";
      } else {
        // GPT Image models (1.5, 1, etc) often support 1024x1536 (3:4) but not 1792 (16:9)
        size = isPortrait ? "1024x1536" : "1536x1024";
      }

      const body = {
        model: aiModel.value,
        prompt: aiPrompt.value,
        n: 1,
        size: size,
      };

      if (isDalle3) {
        // DALL-E 3: quality ("standard" or "hd"), style ("vivid" or "natural")
        body.quality = "hd";
        body.style = "vivid";
        body.response_format = "b64_json";
      } else if (isDalle2) {
        // DALL-E 2: no quality/style params, only response_format
        body.response_format = "b64_json";
      } else {
        // GPT Image models: quality ("low", "medium", "high"), output_format, output_compression
        // Note: GPT Image models return b64_json by default, no response_format needed
        body.quality = "high";
      }

      const response = await fetch("https://api.openai.com/v1/images/generations", {
        method: "POST",
        headers: {
          "Content-Type": "application/json",
          Authorization: `Bearer ${apiKey}`,
        },
        body: JSON.stringify(body),
      });

      if (!response.ok) {
        const errorText = await response.text();
        throw new Error(`API Error: ${response.status} - ${errorText}`);
      }

      const data = await response.json();

      if (data.data?.[0]?.b64_json) {
        // All models return b64_json when requested (DALL-E) or by default (GPT Image)
        src = `data:image/png;base64,${data.data[0].b64_json}`;
      } else if (data.data?.[0]?.url) {
        // Fallback: handle URL response if returned
        const urlRes = await fetch(data.data[0].url);
        if (!urlRes.ok) throw new Error("Failed to download image from OpenAI URL");
        const blob = await urlRes.blob();
        src = URL.createObjectURL(blob);
      } else {
        throw new Error("Invalid response from AI API: missing image data");
      }
    } else {
      // Google Gemini
      // Build imageConfig - imageSize only supported by Gemini 3 Pro
      const imageConfig = {
        aspectRatio: isPortrait ? "3:4" : "4:3",
      };

      if (aiModel.value.includes("gemini-3")) {
        // Select imageSize based on display resolution
        // 1K (~1024px), 2K (~2048px), 4K (~4096px)
        const maxDim = Math.max(displayWidth.value, displayHeight.value);
        if (maxDim > 2048) {
          imageConfig.imageSize = "4K";
        } else if (maxDim > 1024) {
          imageConfig.imageSize = "2K";
        } else {
          imageConfig.imageSize = "1K";
        }
      }

      const response = await fetch(
        `https://generativelanguage.googleapis.com/v1beta/models/${aiModel.value}:generateContent?key=${apiKey}`,
        {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({
            contents: [{ parts: [{ text: aiPrompt.value }] }],
            generationConfig: {
              responseModalities: ["Image"],
              imageConfig: imageConfig,
            },
          }),
        }
      );

      if (!response.ok) {
        const errorText = await response.text();
        throw new Error(`API Error: ${response.status} - ${errorText}`);
      }

      const data = await response.json();
      const b64 = data.candidates?.[0]?.content?.parts?.[0]?.inlineData?.data;
      if (!b64) {
        throw new Error("Invalid response from Google API: missing inlineData");
      }
      src = `data:image/jpeg;base64,${b64}`;
    }

    const res = await fetch(src);
    const blob = await res.blob();
    const timestamp = new Date().toISOString().replace(/[:.]/g, "-");
    const file = new File([blob], `ai-generated-${timestamp}.jpg`, { type: "image/jpeg" });

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
      <v-spacer />
      <template v-if="canSaveToAlbum && appStore.systemInfo.storage_total > 0">
        <v-icon
          :icon="appStore.systemInfo.sdcard_inserted ? 'mdi-sd' : 'mdi-database'"
          size="small"
          class="mr-1"
        />
        <span class="text-body-2 text-medium-emphasis mr-2">
          {{ storageUsedMBString }} / {{ storageTotalMBString }}
        </span>
        <v-chip size="x-small" :color="storageColor" variant="flat" class="text-white">
          {{ storageUsedPercent }}%
        </v-chip>
        <v-btn
          v-if="appStore.systemInfo.has_flash_storage && !appStore.systemInfo.sdcard_inserted"
          icon="mdi-delete-sweep"
          size="x-small"
          variant="text"
          class="ml-1"
          title="Format storage"
          @click="showFormatConfirm = true"
        />
      </template>
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
          ref="imageProcessingRef"
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
    <!-- Format Storage Confirmation Dialog -->
    <v-dialog v-model="showFormatConfirm" max-width="400">
      <v-card>
        <v-card-title>Format Storage</v-card-title>
        <v-card-text>
          This will erase all images on internal storage. This action cannot be undone.
        </v-card-text>
        <v-card-actions>
          <v-spacer />
          <v-btn variant="text" @click="showFormatConfirm = false">Cancel</v-btn>
          <v-btn color="error" :loading="formatting" @click="formatStorage">Format</v-btn>
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
