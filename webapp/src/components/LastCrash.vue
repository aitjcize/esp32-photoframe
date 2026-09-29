<script setup>
import { ref, computed } from "vue";
import { useAppStore } from "../stores";
import { copyText } from "../utils/clipboard";

const appStore = useAppStore();
const crash = computed(() => appStore.systemInfo.last_crash || null);

// The time is when the next boot found the dump, which follows the crash
// within seconds.
const detected = computed(() =>
  crash.value.time
    ? new Date(crash.value.time * 1000).toLocaleString()
    : "Unknown (the frame's clock was not set)"
);

const firmware = computed(() => {
  const { firmware: version, elf_sha256: sha } = crash.value;
  const name = version || "Unknown build";
  return sha ? `${name} (ELF ${sha})` : name;
});

const trace = computed(() => {
  const { task, pc, backtrace, backtrace_corrupted: corrupted } = crash.value;
  const bt = (backtrace || []).join(" ") + (corrupted ? " (corrupted)" : "");
  return `task ${task}\npc   ${pc}\nbt   ${bt}`;
});

const copyState = ref("");
let copyTimer = null;

async function copyReport() {
  try {
    await copyText(crash.value.summary);
    copyState.value = "copied";
  } catch (error) {
    console.error("Failed to copy crash report:", error);
    copyState.value = "failed";
  }
  clearTimeout(copyTimer);
  copyTimer = setTimeout(() => (copyState.value = ""), 3000);
}

const clearing = ref(false);
const clearFailed = ref(false);

async function clearCrash() {
  clearing.value = true;
  clearFailed.value = false;
  try {
    const response = await fetch("/api/crash/clear", { method: "POST" });
    if (!response.ok) {
      throw new Error(`HTTP ${response.status}`);
    }
    appStore.systemInfo.last_crash = null;
  } catch (error) {
    console.error("Failed to clear crash report:", error);
    clearFailed.value = true;
  } finally {
    clearing.value = false;
  }
}
</script>

<template>
  <div v-if="crash">
    <div class="text-subtitle-1 mb-4">Last Crash</div>
    <div class="text-body-2 mb-1"><strong>Detected:</strong> {{ detected }}</div>
    <div class="text-body-2 mb-1"><strong>Firmware:</strong> {{ firmware }}</div>
    <div class="text-body-2 mb-3"><strong>Reason:</strong> {{ crash.reason }}</div>
    <pre class="crash-trace mb-3">{{ trace }}</pre>
    <div class="text-body-2 text-medium-emphasis mb-3">
      Include the copied report when you file a bug, then clear it.
    </div>
    <v-btn variant="outlined" class="mr-2" @click="copyReport">
      <v-icon start>{{ copyState === "copied" ? "mdi-check" : "mdi-content-copy" }}</v-icon>
      {{ copyState === "copied" ? "Copied" : "Copy Report" }}
    </v-btn>
    <v-btn variant="outlined" :loading="clearing" @click="clearCrash">
      <v-icon start>mdi-delete</v-icon>
      Clear
    </v-btn>
    <v-alert
      v-if="copyState === 'failed' || clearFailed"
      type="error"
      variant="tonal"
      density="compact"
      class="mt-3"
    >
      {{
        clearFailed
          ? "Failed to clear the crash report"
          : "Couldn't copy to the clipboard; select the details above instead"
      }}
    </v-alert>
    <v-divider class="my-6" />
  </div>
</template>

<style scoped>
.crash-trace {
  font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
  font-size: 0.85rem;
  white-space: pre-wrap;
  word-break: break-all;
  padding: 8px 12px;
  border-radius: 4px;
  background: rgba(var(--v-theme-on-surface), 0.05);
}
</style>
