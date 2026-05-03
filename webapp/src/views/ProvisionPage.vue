<script setup>
import { ref, onMounted, onUnmounted } from "vue";

// Inline SVG icon paths (works offline during provisioning)
const ICON_PATHS = {
  wifiStrength1:
    "M12 3C7.79 3 3.7 4.41.38 7C4.41 12.06 7.89 16.37 12 21.5c4.08-5.08 8.24-10.26 11.65-14.5C20.32 4.41 16.22 3 12 3m0 2c3.07 0 6.09.86 8.71 2.45l-5.1 6.36A8.4 8.4 0 0 0 12 13c-1.25 0-2.5.28-3.61.8L3.27 7.44C5.91 5.85 8.93 5 12 5",
  wifiStrength2:
    "M12 3C7.79 3 3.7 4.41.38 7C4.41 12.06 7.89 16.37 12 21.5c4.08-5.08 8.24-10.26 11.65-14.5C20.32 4.41 16.22 3 12 3m0 2c3.07 0 6.09.86 8.71 2.45l-3.21 3.98C16.26 10.74 14.37 10 12 10c-2.38 0-4.26.75-5.5 1.43L3.27 7.44C5.91 5.85 8.93 5 12 5",
  wifiStrength3:
    "M12 3C7.79 3 3.7 4.41.38 7C4.41 12.06 7.89 16.37 12 21.5c4.08-5.08 8.24-10.26 11.65-14.5C20.32 4.41 16.22 3 12 3m0 2c3.07 0 6.09.86 8.71 2.45l-1.94 2.43C17.26 9 14.88 8 12 8C9 8 6.68 9 5.21 9.84l-1.94-2.4C5.91 5.85 8.93 5 12 5",
  wifiStrength4:
    "M12 3C7.79 3 3.7 4.41.38 7C4.41 12.06 7.89 16.37 12 21.5c4.08-5.08 8.24-10.26 11.65-14.5C20.32 4.41 16.22 3 12 3",
  refresh:
    "M17.65 6.35A7.96 7.96 0 0 0 12 4a8 8 0 0 0-8 8a8 8 0 0 0 8 8c3.73 0 6.84-2.55 7.73-6h-2.08A5.99 5.99 0 0 1 12 18a6 6 0 0 1-6-6a6 6 0 0 1 6-6c1.66 0 3.14.69 4.22 1.78L13 11h7V4z",
  eye: "M12 9a3 3 0 0 0-3 3a3 3 0 0 0 3 3a3 3 0 0 0 3-3a3 3 0 0 0-3-3m0 8a5 5 0 0 1-5-5a5 5 0 0 1 5-5a5 5 0 0 1 5 5a5 5 0 0 1-5 5m0-12.5C7 4.5 2.73 7.61 1 12c1.73 4.39 6 7.5 11 7.5s9.27-3.11 11-7.5c-1.73-4.39-6-7.5-11-7.5",
  eyeOff:
    "M11.83 9L15 12.16V12a3 3 0 0 0-3-3zm-4.3.8l1.55 1.55c-.05.21-.08.42-.08.65a3 3 0 0 0 3 3c.22 0 .44-.03.65-.08l1.55 1.55c-.67.33-1.41.53-2.2.53a5 5 0 0 1-5-5c0-.79.2-1.53.53-2.2M2 4.27l2.28 2.28l.45.45C3.08 8.3 1.78 10 1 12c1.73 4.39 6 7.5 11 7.5c1.55 0 3.03-.3 4.38-.84l.43.42L19.73 22L21 20.73L3.27 3M12 7a5 5 0 0 1 5 5c0 .64-.13 1.26-.36 1.82l2.93 2.93c1.5-1.25 2.7-2.89 3.43-4.75c-1.73-4.39-6-7.5-11-7.5c-1.4 0-2.74.25-4 .7l2.17 2.15C10.74 7.13 11.35 7 12 7",
};

const mode = ref("sta"); // 'sta' | 'ap'
const ssid = ref("");
const password = ref("");
const deviceName = ref("PhotoFrame");
const showPassword = ref(false);
const loading = ref(false);
const status = ref(null); // 'success' | 'error' | 'info' | null
const statusMessage = ref("");

const networks = ref([]);
const scanning = ref(false);

function signalIcon(rssi) {
  if (rssi >= -50) return ICON_PATHS.wifiStrength4;
  if (rssi >= -60) return ICON_PATHS.wifiStrength3;
  if (rssi >= -70) return ICON_PATHS.wifiStrength2;
  return ICON_PATHS.wifiStrength1;
}

async function scanNetworks() {
  scanning.value = true;
  try {
    const response = await fetch("/api/wifi/scan");
    if (response.ok) {
      const data = await response.json();
      networks.value = data.map((n) => ({
        title: n.ssid,
        subtitle: `${n.rssi} dBm · ${n.auth}`,
        value: n.ssid,
        rssi: n.rssi,
        iconPath: signalIcon(n.rssi),
      }));
    }
  } catch (e) {
    console.error("WiFi scan failed:", e);
  } finally {
    scanning.value = false;
  }
}

let keepAliveInterval = null;

onMounted(() => {
  scanNetworks();
  keepAliveInterval = setInterval(() => {
    fetch("/api/keep_alive").catch(() => {});
  }, 60000);
});

onUnmounted(() => {
  if (keepAliveInterval) {
    clearInterval(keepAliveInterval);
    keepAliveInterval = null;
  }
});

async function submitForm() {
  loading.value = true;
  status.value = "info";

  const formData = new URLSearchParams();
  formData.append("deviceName", deviceName.value);

  let endpoint;
  if (mode.value === "ap") {
    statusMessage.value = "Configuring standalone hotspot...";
    endpoint = "/save-ap";
  } else {
    statusMessage.value = "Testing WiFi connection...";
    formData.append("ssid", ssid.value);
    formData.append("password", password.value);
    endpoint = "/save";
  }

  try {
    const response = await fetch(endpoint, {
      method: "POST",
      headers: {
        "Content-Type": "application/x-www-form-urlencoded",
      },
      body: formData.toString(),
    });

    if (response.ok) {
      status.value = "success";

      if (mode.value === "ap") {
        statusMessage.value =
          "Standalone hotspot configured! The device is restarting and will come up as its own WiFi network. Watch the device screen for the new SSID and password.";
      } else {
        const hostname = deviceName.value.toLowerCase().replace(/\s+/g, "-");
        statusMessage.value = `Credentials saved! Device will restart in 3 seconds and attempt to connect to "${ssid.value}".`;

        setTimeout(() => {
          statusMessage.value += `\n\nRestarting now... Close this page and reconnect to your WiFi network, then visit http://${hostname}.local`;
        }, 3000);
      }
    } else {
      loading.value = false;
      status.value = "error";

      const text = await response.text();
      if (text.includes("Connection Failed") || text.includes("connect")) {
        statusMessage.value =
          "WiFi Connection Failed. Please check your password, SSID, and ensure it's a 2.4GHz network.";
      } else {
        statusMessage.value = "An error occurred. Please try again.";
      }
    }
  } catch (error) {
    loading.value = false;
    status.value = "error";
    statusMessage.value = "Error: " + error.message;
  }
}
</script>

<template>
  <v-app>
    <v-main class="provision-main bg-grey-lighten-4">
      <v-card class="provision-card" elevation="12">
        <v-card-title class="text-h6"> PhotoFrame Setup </v-card-title>

        <v-card-subtitle class="mb-2"> Choose how the PhotoFrame connects </v-card-subtitle>

        <v-form @submit.prevent="submitForm">
          <v-radio-group
            v-model="mode"
            :disabled="loading"
            class="mb-2"
            hide-details
            density="comfortable"
          >
            <v-radio value="sta">
              <template #label>
                <div>
                  <div class="text-body-2 font-weight-medium">Connect to existing WiFi</div>
                  <div class="text-caption text-medium-emphasis">
                    Joins your home network. Enables NTP, OTA, Home Assistant, and URL-based
                    rotation.
                  </div>
                </div>
              </template>
            </v-radio>
            <v-radio value="ap">
              <template #label>
                <div>
                  <div class="text-body-2 font-weight-medium">Standalone hotspot</div>
                  <div class="text-caption text-medium-emphasis">
                    Device runs its own WiFi for direct phone access. No internet —
                    internet-dependent features will be disabled.
                  </div>
                </div>
              </template>
            </v-radio>
          </v-radio-group>

          <v-combobox
            v-if="mode === 'sta'"
            v-model="ssid"
            :items="networks"
            item-title="title"
            item-value="value"
            label="WiFi Network Name (SSID)"
            variant="outlined"
            required
            :disabled="loading"
            :loading="scanning"
            class="mb-2"
            placeholder="Select or type a network name"
            @update:model-value="
              (val) => {
                if (val && typeof val === 'object') ssid = val.value;
              }
            "
          >
            <template #item="{ item, props: itemProps }">
              <v-list-item v-bind="itemProps">
                <template #prepend>
                  <svg viewBox="0 0 24 24" class="wifi-icon mr-2">
                    <path :d="item.raw.iconPath" fill="currentColor" />
                  </svg>
                </template>
                <template #append>
                  <span class="text-caption text-medium-emphasis">{{ item.raw.subtitle }}</span>
                </template>
              </v-list-item>
            </template>
            <template #append>
              <v-btn
                variant="text"
                size="small"
                :loading="scanning"
                :disabled="loading"
                @click.stop="scanNetworks"
              >
                <svg viewBox="0 0 24 24" class="btn-icon">
                  <path :d="ICON_PATHS.refresh" fill="currentColor" />
                </svg>
              </v-btn>
            </template>
          </v-combobox>

          <v-text-field
            v-if="mode === 'sta'"
            v-model="password"
            label="WiFi Password"
            :type="showPassword ? 'text' : 'password'"
            variant="outlined"
            :disabled="loading"
            hint="Leave blank for open networks"
            class="mb-2"
          >
            <template #append-inner>
              <span class="password-toggle" @click="showPassword = !showPassword">
                <svg viewBox="0 0 24 24" class="btn-icon">
                  <path
                    :d="showPassword ? ICON_PATHS.eyeOff : ICON_PATHS.eye"
                    fill="currentColor"
                  />
                </svg>
              </span>
            </template>
          </v-text-field>

          <v-text-field
            v-model="deviceName"
            label="Device Name"
            variant="outlined"
            :disabled="loading"
            hint="Used for mDNS hostname"
            class="mb-4"
          />

          <v-btn type="submit" color="primary" size="large" block :loading="loading">
            {{ mode === "ap" ? "Set up hotspot" : "Connect to WiFi" }}
          </v-btn>
        </v-form>

        <v-alert v-if="status" :type="status" variant="tonal" class="mt-4" :icon="false">
          <div style="white-space: pre-line">{{ statusMessage }}</div>
        </v-alert>
      </v-card>
    </v-main>
  </v-app>
</template>

<style scoped>
.provision-main {
  min-height: 100vh;
  padding: 16px;
  display: flex;
  align-items: flex-start;
  justify-content: center;
  padding-top: 40px;
}

.provision-card {
  width: 100%;
  max-width: 380px;
  padding: 20px;
}

.wifi-icon {
  width: 20px;
  height: 20px;
}

.btn-icon {
  width: 22px;
  height: 22px;
}

.password-toggle {
  cursor: pointer;
  user-select: none;
  display: flex;
  align-items: center;
  opacity: 0.6;
}

.password-toggle:hover {
  opacity: 1;
}
</style>
