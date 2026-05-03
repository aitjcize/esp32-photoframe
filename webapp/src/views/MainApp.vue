<script setup>
import { onMounted } from "vue";
import { useAppStore, useSettingsStore } from "../stores";
import AppHeader from "../components/AppHeader.vue";
import AlbumGallery from "../components/AlbumGallery.vue";
import ImageUpload from "../components/ImageUpload.vue";
import SettingsPanel from "../components/SettingsPanel.vue";
import OtaUpdate from "../components/OtaUpdate.vue";

const appStore = useAppStore();
const settingsStore = useSettingsStore();

onMounted(async () => {
  // Load system info first to check for SD card
  await appStore.loadSystemInfo();

  await Promise.all([
    appStore.loadBatteryStatus(),
    appStore.loadAlbums(),
    settingsStore.loadSettings(),
    settingsStore.loadDeviceSettings(),
    settingsStore.loadPalette(),
  ]);
  appStore.selectAlbum("Default");

  // Refresh battery every 30s
  setInterval(() => appStore.loadBatteryStatus(), 30000);
});
</script>

<template>
  <v-app>
    <AppHeader />

    <v-main class="bg-grey-lighten-4">
      <v-container class="py-6" style="max-width: 1200px">
        <v-alert
          v-if="
            appStore.systemInfo.has_sdcard &&
            !appStore.systemInfo.sdcard_inserted &&
            !appStore.systemInfo.has_flash_storage
          "
          type="info"
          variant="tonal"
          class="mb-6"
          title="SD Card Not Detected"
          text="This device supports an SD card. To upload images and create multiple albums, please insert one and restart the device."
        ></v-alert>

        <AlbumGallery
          v-if="appStore.systemInfo.sdcard_inserted || appStore.systemInfo.has_flash_storage"
        />

        <ImageUpload class="mt-6" />

        <SettingsPanel class="mt-6" />

        <OtaUpdate v-if="!appStore.isApMode" class="mt-6" />
      </v-container>
    </v-main>

    <v-footer app class="text-center d-flex justify-center">
      <span class="text-body-2 text-grey">
        {{ appStore.systemInfo.project_name }} {{ appStore.systemInfo.version }}
      </span>
    </v-footer>
  </v-app>
</template>
