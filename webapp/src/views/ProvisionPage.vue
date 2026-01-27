<script setup>
import { ref } from "vue";

const ssid = ref("");
const password = ref("");
const deviceName = ref("PhotoFrame");
const showPassword = ref(false);
const loading = ref(false);
const status = ref(null); // 'success' | 'error' | 'info' | null
const statusMessage = ref("");

async function submitForm() {
  loading.value = true;
  status.value = "info";
  statusMessage.value = "Testing WiFi connection...";

  const formData = new URLSearchParams();
  formData.append("ssid", ssid.value);
  formData.append("password", password.value);
  formData.append("deviceName", deviceName.value);

  try {
    const response = await fetch("/save", {
      method: "POST",
      headers: {
        "Content-Type": "application/x-www-form-urlencoded",
      },
      body: formData.toString(),
    });

    if (response.ok) {
      status.value = "success";
      // Generate mDNS hostname from device name (lowercase, spaces to hyphens)
      const hostname = deviceName.value.toLowerCase().replace(/\s+/g, "-");
      statusMessage.value = `Credentials saved! Device will restart in 3 seconds and attempt to connect to "${ssid.value}".`;

      setTimeout(() => {
        statusMessage.value +=
          `\n\nRestarting now... Close this page and reconnect to your WiFi network, then visit http://${hostname}.local`;
      }, 3000);
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
        <v-card-title class="text-h6">
          PhotoFrame Setup
        </v-card-title>

        <v-card-subtitle class="mb-2"> Connect your PhotoFrame to WiFi </v-card-subtitle>

        <v-form @submit.prevent="submitForm">
          <v-text-field
            v-model="ssid"
            label="WiFi Network Name (SSID)"
            variant="outlined"
            required
            :disabled="loading"
            class="mb-2"
          />

          <v-text-field
            v-model="password"
            label="WiFi Password"
            :type="showPassword ? 'text' : 'password'"
            variant="outlined"
            :disabled="loading"
            hint="Leave blank for open networks"
            class="mb-2"
          >
            <template #append-inner>
              <span
                style="cursor: pointer; user-select: none"
                @click="showPassword = !showPassword"
              >
                {{ showPassword ? "🙈" : "👁️" }}
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
            Connect to WiFi
          </v-btn>
        </v-form>

        <v-alert
          v-if="status"
          :type="status"
          variant="tonal"
          class="mt-4"
          style="white-space: pre-line"
        >
          {{ statusMessage }}
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
</style>
