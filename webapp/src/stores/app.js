import { defineStore } from "pinia";
import { ref, computed } from "vue";

export const useAppStore = defineStore("app", () => {
  // State
  const albums = ref([]);
  const selectedAlbum = ref("Default");
  const images = ref([]);
  const boardOptions = [
    { label: 'Waveshare 7.3" (7-color)', value: "waveshare_photopainter_73" },
    { label: "Seeed Studio XIAO EE02", value: "seeedstudio_xiao_ee02" },
  ];
  const battery = ref({
    connected: false,
    level: 0,
    voltage: 0,
    charging: false,
  });
  const systemInfo = ref({
    width: 800,
    height: 480,
    has_sdcard: false,
    version: "v1.0",
    project_name: "PhotoFrame",
    compile_time: "",
    compile_date: "",
    idf_version: "",
    board_name: "waveshare_photopainter_73",
  });
  const loading = ref({
    albums: false,
    images: false,
    battery: false,
    systemInfo: false,
  });

  // API base URL (empty for same-origin)
  const API_BASE = "";

  // Getters
  const sortedAlbums = computed(() => {
    return [...albums.value].sort((a, b) => {
      if (a.name === "Default") return -1;
      if (b.name === "Default") return 1;
      return a.name.localeCompare(b.name);
    });
  });

  const currentAlbumImages = computed(() => images.value);

  // Actions
  async function loadBatteryStatus() {
    try {
      const response = await fetch(`${API_BASE}/api/battery`);
      if (!response.ok || response.headers.get("content-type")?.includes("text/html")) {
        return;
      }
      const data = await response.json();
      battery.value = {
        connected: data.battery_connected,
        level: data.battery_level,
        voltage: data.battery_voltage,
        charging: data.charging,
      };
    } catch (error) {
      console.log("Battery API not available (standalone mode)");
    }
  }

  async function loadAlbums() {
    loading.value.albums = true;
    try {
      if (!systemInfo.value.has_sdcard) {
        albums.value = [{ name: "Default", enabled: true, image_count: 0 }];
        return;
      }
      const response = await fetch(`${API_BASE}/api/albums`);
      if (!response.ok || response.headers.get("content-type")?.includes("text/html")) {
        albums.value = [{ name: "Default", enabled: true, image_count: 0 }];
        return;
      }
      albums.value = await response.json();
    } catch (error) {
      console.log("Failed to load albums (standalone mode):", error);
      albums.value = [{ name: "Default", enabled: true, image_count: 0 }];
    } finally {
      loading.value.albums = false;
    }
  }

  async function loadImages(albumName) {
    loading.value.images = true;
    try {
      if (!systemInfo.value.has_sdcard) {
        images.value = [];
        return;
      }
      const response = await fetch(`${API_BASE}/api/images?album=${encodeURIComponent(albumName)}`);
      if (!response.ok || response.headers.get("content-type")?.includes("text/html")) {
        images.value = [];
        return;
      }
      images.value = await response.json();
    } catch (error) {
      console.log("Failed to load images (standalone mode):", error);
      images.value = [];
    } finally {
      loading.value.images = false;
    }
  }

  function selectAlbum(albumName) {
    selectedAlbum.value = albumName;
    loadImages(albumName);
  }

  async function toggleAlbumEnabled(albumName, enabled) {
    try {
      const response = await fetch(
        `${API_BASE}/api/albums/enabled?name=${encodeURIComponent(albumName)}`,
        {
          method: "PUT",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ enabled }),
        }
      );
      if (response.ok) {
        await loadAlbums();
      }
    } catch (error) {
      console.error("Failed to toggle album:", error);
    }
  }

  async function createAlbum(name) {
    try {
      const response = await fetch(`${API_BASE}/api/albums`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ name: name.trim() }),
      });
      if (response.ok) {
        await loadAlbums();
        return true;
      }
      return false;
    } catch (error) {
      console.error("Failed to create album:", error);
      return false;
    }
  }

  async function deleteAlbum(albumName) {
    try {
      const response = await fetch(`${API_BASE}/api/albums?name=${encodeURIComponent(albumName)}`, {
        method: "DELETE",
      });
      if (response.ok) {
        await loadAlbums();
        if (selectedAlbum.value === albumName) {
          selectAlbum("Default");
        }
        return true;
      }
      return false;
    } catch (error) {
      console.error("Failed to delete album:", error);
      return false;
    }
  }

  async function deleteImage(album, filename) {
    try {
      const fullPath = `${album}/${filename}`;
      const response = await fetch(`${API_BASE}/api/delete`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ filepath: fullPath }),
      });
      if (response.ok) {
        await loadImages(selectedAlbum.value);
        return true;
      }
      return false;
    } catch (error) {
      console.error("Failed to delete image:", error);
      return false;
    }
  }

  async function displayImage(album, filename) {
    try {
      const fullPath = `${album}/${filename}`;
      const response = await fetch(`${API_BASE}/api/display`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ filepath: fullPath }),
      });
      return response.ok;
    } catch (error) {
      console.error("Failed to display image:", error);
      return false;
    }
  }

  async function enterSleep() {
    try {
      const response = await fetch(`${API_BASE}/api/sleep`, { method: "POST" });
      return response.ok;
    } catch (error) {
      console.error("Failed to enter sleep:", error);
      return false;
    }
  }

  async function rotateNow() {
    try {
      const response = await fetch(`${API_BASE}/api/rotate`, { method: "POST" });
      return response.ok;
    } catch (error) {
      console.error("Failed to rotate:", error);
      return false;
    }
  }

  async function loadSystemInfo() {
    loading.value.systemInfo = true;
    try {
      const response = await fetch(`${API_BASE}/api/system-info`);
      if (response.ok) {
        systemInfo.value = await response.json();
      }
    } catch (error) {
      console.error("Failed to load system info:", error);
    } finally {
      loading.value.systemInfo = false;
    }
  }

  return {
    // State
    albums,
    selectedAlbum,
    images,
    battery,
    systemInfo,
    loading,
    // Getters
    sortedAlbums,
    currentAlbumImages,
    // Actions
    loadBatteryStatus,
    loadAlbums,
    loadImages,
    selectAlbum,
    toggleAlbumEnabled,
    createAlbum,
    deleteAlbum,
    deleteImage,
    displayImage,
    enterSleep,
    rotateNow,
    loadSystemInfo,
  };
});
