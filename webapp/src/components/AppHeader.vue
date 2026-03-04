<script setup>
import { ref, computed } from "vue";
import { useAppStore } from "../stores";

const appStore = useAppStore();
const sleepDialog = ref(false);
const rotating = ref(false);

async function handleSleep() {
  await appStore.enterSleep();
  sleepDialog.value = false;
}

async function handleRotate() {
  rotating.value = true;
  await appStore.rotateNow();
  rotating.value = false;
}

function getBatteryColor(level) {
  if (level < 20) return "error";
  if (level < 50) return "warning";
  return "success";
}

const formatStorageBytes = (bytes) => {
  if (!bytes) return "0 MB";
  return (bytes / 1024 / 1024).toFixed(1) + " MB";
};

const storageUsedMBString = computed(() => formatStorageBytes(appStore.systemInfo.storage_used));
const storageTotalMBString = computed(() => formatStorageBytes(appStore.systemInfo.storage_total));

const storageColor = computed(() => {
  const total = appStore.systemInfo.storage_total || 1;
  const used = appStore.systemInfo.storage_used || 0;
  const ratio = used / total;
  if (ratio > 0.9) return "error";
  if (ratio > 0.75) return "warning";
  return "success";
});

const storageIcon = computed(() => {
  // If we compile with SD card support and it's physically there, we could guess it's an SD card.
  // We'll just differentiate visually loosely or fall back to an internal storage icon.
  return "mdi-database";
});
</script>

<template>
  <v-app-bar color="primary" density="comfortable">
    <template #prepend>
      <v-img src="/favicon.svg" alt="PhotoFrame" width="40" height="40" class="ml-2 mr-n2" />
    </template>

    <v-app-bar-title class="ml-4">PhotoFrame Control Panel</v-app-bar-title>

    <template #append>
      <!-- Rotate Now Button -->
      <v-btn
        icon
        class="mr-2"
        :loading="rotating"
        title="Rotate to next image"
        @click="handleRotate"
      >
        <v-icon>mdi-rotate-right</v-icon>
      </v-btn>

      <!-- Sleep Button -->
      <v-btn icon class="mr-2" title="Enter Deep Sleep" @click="sleepDialog = true">
        <v-icon>mdi-sleep</v-icon>
      </v-btn>

      <!-- Storage Status -->
      <v-chip
        v-if="appStore.systemInfo.sdcard_inserted && appStore.systemInfo.storage_total > 0"
        variant="flat"
        :color="storageColor"
        class="mr-2 text-white"
      >
        <v-icon :icon="storageIcon" start />
        {{ storageUsedMBString }} / {{ storageTotalMBString }}
      </v-chip>

      <!-- Battery Status -->
      <v-chip
        v-if="appStore.battery.connected"
        variant="flat"
        :color="getBatteryColor(appStore.battery.level)"
        class="text-white"
      >
        <v-icon :icon="appStore.battery.charging ? 'mdi-battery-charging' : 'mdi-battery'" start />
        {{ appStore.battery.level }}%
        <span v-if="appStore.battery.charging" class="ml-1">Charging</span>
      </v-chip>
      <v-chip v-else variant="flat" color="grey-darken-1" class="mr-2 text-white">
        <v-icon icon="mdi-power-plug-off" start />
        No Battery
      </v-chip>
    </template>
  </v-app-bar>

  <!-- Sleep Confirmation Dialog -->
  <v-dialog v-model="sleepDialog" max-width="400">
    <v-card>
      <v-card-title>Enter Deep Sleep?</v-card-title>
      <v-card-text>
        The device will enter deep sleep mode and stop responding until woken up.
      </v-card-text>
      <v-card-actions>
        <v-spacer />
        <v-btn variant="text" @click="sleepDialog = false"> Cancel </v-btn>
        <v-btn color="primary" @click="handleSleep"> Sleep </v-btn>
      </v-card-actions>
    </v-card>
  </v-dialog>
</template>
