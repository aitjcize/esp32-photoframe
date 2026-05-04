<script setup>
import { ref, computed, onMounted, onUnmounted } from "vue";

const currentVersion = ref("Loading...");
const latestVersion = ref("-");
const otaState = ref("idle");
const progress = ref(0);
const errorMessage = ref("");

let statusPollInterval = null;

const updateAvailable = computed(() => otaState.value === "update_available");
const checking = computed(() => otaState.value === "checking");
const installing = computed(
  () => otaState.value === "downloading" || otaState.value === "installing"
);

const statusMessage = computed(() => {
  switch (otaState.value) {
    case "idle":
      return "";
    case "checking":
      return "Checking for updates...";
    case "update_available":
      return `Update available: ${latestVersion.value}`;
    case "downloading":
      return "Downloading firmware...";
    case "installing":
      return "Installing firmware...";
    case "success":
      return "Update successful! Device will reboot...";
    case "error":
      return errorMessage.value || "Update failed";
    case "up_to_date":
      return "You're running the latest version.";
    default:
      return "";
  }
});

const statusType = computed(() => {
  switch (otaState.value) {
    case "update_available":
    case "success":
      return "success";
    case "error":
      return "error";
    case "checking":
    case "downloading":
    case "installing":
      return "info";
    default:
      return "info";
  }
});

async function loadOTAStatus() {
  try {
    const response = await fetch("/api/ota/status");
    if (!response.ok) return;

    const data = await response.json();

    currentVersion.value = data.current_version || "Unknown";
    latestVersion.value = data.latest_version || "-";
    otaState.value = data.state || "idle";
    progress.value = data.progress_percent || 0;
    errorMessage.value = data.error_message || "";

    // If state is idle and we were checking, it means check completed with no update
    // The state should transition appropriately based on the backend response
  } catch (error) {
    console.error("Failed to load OTA status:", error);
  }
}

async function checkForUpdate() {
  try {
    const response = await fetch("/api/ota/check", { method: "POST" });
    if (!response.ok) {
      throw new Error(`HTTP ${response.status}`);
    }

    // Start polling for status updates
    startStatusPolling();

    // Immediately reload status
    await loadOTAStatus();
  } catch (error) {
    errorMessage.value = "Failed to check for updates: " + error.message;
    otaState.value = "error";
  }
}

async function installUpdate() {
  try {
    const response = await fetch("/api/ota/update", { method: "POST" });
    if (!response.ok) {
      throw new Error(`HTTP ${response.status}`);
    }

    // Start polling for status updates
    startStatusPolling();

    // Immediately reload status
    await loadOTAStatus();
  } catch (error) {
    errorMessage.value = "Failed to install update: " + error.message;
    otaState.value = "error";
  }
}

function startStatusPolling() {
  // Clear any existing interval
  stopStatusPolling();

  // Poll every second
  statusPollInterval = setInterval(async () => {
    await loadOTAStatus();

    // Stop polling if we're in a terminal state
    if (
      otaState.value === "idle" ||
      otaState.value === "update_available" ||
      otaState.value === "success" ||
      otaState.value === "error" ||
      otaState.value === "up_to_date"
    ) {
      stopStatusPolling();
    }
  }, 1000);
}

function stopStatusPolling() {
  if (statusPollInterval) {
    clearInterval(statusPollInterval);
    statusPollInterval = null;
  }
}

onMounted(() => {
  loadOTAStatus();
});

onUnmounted(() => {
  stopStatusPolling();
});
</script>

<template>
  <v-card style="overflow: visible">
    <v-card-title class="d-flex align-center">
      <v-icon icon="$mdi-cellphone-arrow-down" class="mr-2" />
      Firmware Update (OTA)
    </v-card-title>

    <v-card-text>
      <p class="text-body-2 text-grey mb-4">
        Check for and install firmware updates from GitHub releases.
      </p>

      <v-row align="center">
        <v-col cols="12" md="4">
          <v-list-item>
            <v-list-item-title class="text-body-2 text-grey"> Current Version </v-list-item-title>
            <v-list-item-subtitle class="text-h6">
              {{ currentVersion }}
            </v-list-item-subtitle>
          </v-list-item>
        </v-col>

        <v-col cols="12" md="4">
          <v-list-item>
            <v-list-item-title class="text-body-2 text-grey"> Latest Version </v-list-item-title>
            <v-list-item-subtitle class="text-h6">
              <span :class="{ 'text-success': updateAvailable }">
                {{ latestVersion }}
              </span>
              <v-chip v-if="updateAvailable" color="success" size="small" class="ml-2">
                Update Available
              </v-chip>
            </v-list-item-subtitle>
          </v-list-item>
        </v-col>

        <v-col cols="12" md="4" class="d-flex flex-wrap justify-end gap-2">
          <v-btn variant="outlined" :loading="checking" class="mr-2" @click="checkForUpdate">
            <v-icon icon="$mdi-refresh" start />
            Check
          </v-btn>

          <v-btn
            v-if="updateAvailable"
            color="primary"
            :loading="installing"
            @click="installUpdate"
          >
            <v-icon icon="$mdi-download" start />
            Install
          </v-btn>
        </v-col>
      </v-row>

      <!-- Progress Bar -->
      <v-expand-transition>
        <div v-if="installing" class="mt-4">
          <v-progress-linear :model-value="progress" color="primary" height="8" rounded />
          <p class="text-body-2 text-center mt-2">{{ progress }}%</p>
        </div>
      </v-expand-transition>

      <!-- Status Message -->
      <v-alert
        v-if="statusMessage"
        :type="statusType"
        variant="tonal"
        class="mt-4"
        density="compact"
      >
        {{ statusMessage }}
      </v-alert>
    </v-card-text>
  </v-card>
</template>
