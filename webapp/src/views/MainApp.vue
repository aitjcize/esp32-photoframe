<script setup>
import { computed, onMounted, onUnmounted, ref } from "vue";
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

// Keep the device awake while the webapp is open and focused.
// Pause polling when the tab is hidden, and ping immediately on re-focus.
// If a ping fails the device has gone to deep sleep (unreachable), so we
// surface a hint telling the user which physical button wakes their board.
const KEEP_ALIVE_INTERVAL = 30000;
const WAKE_RETRY_INTERVAL = 5000;
let keepAliveInterval = null;
let wakeRetryInterval = null;

const deviceAsleep = ref(false);
const wakeupKeyName = computed(() => appStore.systemInfo.wakeup_key_name || "wake button");

async function pingKeepAlive() {
  const alive = await appStore.keepAlive();
  deviceAsleep.value = !alive;
  if (alive) {
    stopWakeRetry();
  } else {
    startWakeRetry();
  }
  return alive;
}

// While the device is unreachable, poll faster so the hint clears promptly
// once the user presses the wakeup button.
function startWakeRetry() {
  if (wakeRetryInterval) return;
  wakeRetryInterval = setInterval(pingKeepAlive, WAKE_RETRY_INTERVAL);
}

function stopWakeRetry() {
  if (wakeRetryInterval) {
    clearInterval(wakeRetryInterval);
    wakeRetryInterval = null;
  }
}

function startKeepAlive() {
  if (keepAliveInterval) return;
  pingKeepAlive();
  keepAliveInterval = setInterval(pingKeepAlive, KEEP_ALIVE_INTERVAL);
}

function stopKeepAlive() {
  if (keepAliveInterval) {
    clearInterval(keepAliveInterval);
    keepAliveInterval = null;
  }
}

function handleVisibilityChange() {
  if (document.visibilityState === "visible") {
    startKeepAlive();
  } else {
    // Don't poll while hidden/unfocused.
    stopKeepAlive();
    stopWakeRetry();
  }
}

onMounted(() => {
  document.addEventListener("visibilitychange", handleVisibilityChange);
  if (document.visibilityState === "visible") {
    startKeepAlive();
  }
});

onUnmounted(() => {
  document.removeEventListener("visibilitychange", handleVisibilityChange);
  stopKeepAlive();
  stopWakeRetry();
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

        <OtaUpdate class="mt-6" />
      </v-container>
    </v-main>

    <!-- Device-asleep hint: shown when the device stops responding to keep-alive -->
    <v-dialog v-model="deviceAsleep" max-width="440" persistent>
      <v-card>
        <v-card-title class="d-flex align-center">
          <v-icon icon="mdi-power-sleep" class="mr-2" />
          Device is asleep
        </v-card-title>
        <v-card-text>
          Your photo frame has gone to sleep to save power and is no longer responding. Press the
          <strong>{{ wakeupKeyName }}</strong>
          on the device to wake it, then tap Try again.
        </v-card-text>
        <v-card-actions>
          <v-spacer />
          <v-btn color="primary" variant="text" @click="pingKeepAlive"> Try again </v-btn>
        </v-card-actions>
      </v-card>
    </v-dialog>

    <v-footer app class="text-center d-flex justify-center">
      <span class="text-body-2 text-grey">
        {{ appStore.systemInfo.project_name }} {{ appStore.systemInfo.version }}
      </span>
    </v-footer>
  </v-app>
</template>
