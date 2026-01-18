import {
  processImage,
  applyExposure,
  applyContrast,
  applySaturation,
  applyScurveTonemap,
  applyExifOrientation,
  resizeImageCover,
  generateThumbnail,
} from "./image-processor.js";

const API_BASE = "";

// Debounced save timer (10 seconds)
let settingsSaveTimer = null;
const SETTINGS_SAVE_DELAY = 10000; // 10 seconds

let currentImages = [];
let selectedImage = null;
let currentAlbums = [];
let selectedAlbum = "Default";

async function loadBatteryStatus() {
  try {
    const response = await fetch(`${API_BASE}/api/battery`);
    if (
      !response.ok ||
      response.headers.get("content-type")?.includes("text/html")
    ) {
      // Running in standalone mode without ESP32 backend
      return;
    }
    const data = await response.json();

    const batteryDiv = document.getElementById("batteryStatus");

    if (!data.battery_connected) {
      batteryDiv.innerHTML =
        '<span class="battery-disconnected">🔌 No Battery</span>';
      return;
    }

    const percent = data.battery_level;
    const voltage = data.battery_voltage;
    const charging = data.charging;

    let batteryIcon = "🔋";
    let batteryClass = "battery-normal";

    if (charging) {
      batteryIcon = "⚡";
      batteryClass = "battery-charging";
    } else if (percent < 20) {
      batteryClass = "battery-low";
    } else if (percent < 50) {
      batteryClass = "battery-medium";
    }

    batteryDiv.innerHTML = `
            <span class="${batteryClass}">
                ${batteryIcon} ${percent}% (${(voltage / 1000).toFixed(2)}V)
                ${charging ? " Charging" : ""}
            </span>
        `;
  } catch (error) {
    // Silently fail if API not available (standalone mode)
    console.log("Battery API not available (standalone mode)");
  }
}

async function loadAlbums() {
  try {
    const response = await fetch(`${API_BASE}/api/albums`);
    if (
      !response.ok ||
      response.headers.get("content-type")?.includes("text/html")
    ) {
      console.log("Albums API not available (standalone mode)");
      // Create default album for local testing
      currentAlbums = [{ name: "Default", enabled: true, image_count: 0 }];
      displayAlbums();
      return;
    }
    currentAlbums = await response.json();
    displayAlbums();
  } catch (error) {
    console.log("Failed to load albums (standalone mode):", error);
    // Create default album for local testing
    currentAlbums = [{ name: "Default", enabled: true, image_count: 0 }];
    displayAlbums();
  }
}

function displayAlbums() {
  const albumList = document.getElementById("albumList");
  if (!albumList) return;

  albumList.innerHTML = "";

  // Sort albums: Default first, then alphabetically
  const sortedAlbums = [...currentAlbums].sort((a, b) => {
    if (a.name === "Default") return -1;
    if (b.name === "Default") return 1;
    return a.name.localeCompare(b.name);
  });

  // Update upload album selector
  const uploadAlbumSelect = document.getElementById("uploadAlbumSelect");
  if (uploadAlbumSelect) {
    uploadAlbumSelect.innerHTML = "";
    sortedAlbums.forEach((album) => {
      const option = document.createElement("option");
      option.value = album.name;
      option.textContent = album.name;
      if (album.name === selectedAlbum) {
        option.selected = true;
      }
      uploadAlbumSelect.appendChild(option);
    });
  }

  sortedAlbums.forEach((album) => {
    const item = document.createElement("div");
    item.className = "album-item";
    if (album.name === selectedAlbum) {
      item.classList.add("selected");
    }

    const checkbox = document.createElement("input");
    checkbox.type = "checkbox";
    checkbox.checked = album.enabled;
    checkbox.addEventListener("click", (e) => {
      e.stopPropagation();
    });
    checkbox.addEventListener("change", (e) => {
      toggleAlbumEnabled(album.name, e.target.checked);
    });

    const name = document.createElement("span");
    name.textContent = album.name;

    const deleteBtn = document.createElement("button");
    deleteBtn.className = "delete-btn";
    deleteBtn.textContent = "×";
    deleteBtn.title = "Delete album";
    if (album.name === "Default") {
      deleteBtn.disabled = true;
      deleteBtn.style.visibility = "hidden";
    }
    deleteBtn.addEventListener("click", (e) => {
      e.stopPropagation();
      deleteAlbum(album.name);
    });

    item.appendChild(checkbox);
    item.appendChild(name);
    item.appendChild(deleteBtn);
    item.addEventListener("click", () => selectAlbum(album.name));

    albumList.appendChild(item);
  });
}

function selectAlbum(albumName) {
  selectedAlbum = albumName;
  displayAlbums();
  loadImagesForAlbum(albumName);

  // Update album name display
  const albumNameElement = document.getElementById("currentAlbumName");
  if (albumNameElement) {
    albumNameElement.textContent = `${albumName}`;
  }

  // Update upload album selector
  const uploadAlbumSelect = document.getElementById("uploadAlbumSelect");
  if (uploadAlbumSelect) {
    uploadAlbumSelect.value = albumName;
  }
}

async function loadImagesForAlbum(albumName) {
  try {
    const response = await fetch(
      `${API_BASE}/api/images?album=${encodeURIComponent(albumName)}`,
    );
    if (
      !response.ok ||
      response.headers.get("content-type")?.includes("text/html")
    ) {
      console.log("Images API not available (standalone mode)");
      currentImages = [];
      displayImages();
      return;
    }
    currentImages = await response.json();
    displayImages();
  } catch (error) {
    console.log("Failed to load images (standalone mode):", error);
    currentImages = [];
    displayImages();
  }
}

async function toggleAlbumEnabled(albumName, enabled) {
  try {
    const response = await fetch(
      `${API_BASE}/api/albums/enabled?name=${encodeURIComponent(albumName)}`,
      {
        method: "PUT",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ enabled }),
      },
    );
    if (response.ok) {
      console.log(`Album ${albumName} ${enabled ? "enabled" : "disabled"}`);
      loadAlbums();
    }
  } catch (error) {
    console.error("Failed to toggle album:", error);
  }
}

async function createAlbum() {
  const name = prompt("Enter album name:");
  if (!name || name.trim() === "") return;

  try {
    const response = await fetch(`${API_BASE}/api/albums`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ name: name.trim() }),
    });
    if (response.ok) {
      console.log("Album created:", name);
      loadAlbums();
    } else {
      alert("Failed to create album");
    }
  } catch (error) {
    console.error("Failed to create album:", error);
  }
}

async function deleteAlbum(albumName) {
  if (albumName === "Default") {
    alert("Cannot delete Default album");
    return;
  }
  if (!confirm(`Delete album "${albumName}" and all its images?`)) return;

  try {
    const response = await fetch(
      `${API_BASE}/api/albums?name=${encodeURIComponent(albumName)}`,
      {
        method: "DELETE",
      },
    );
    if (response.ok) {
      console.log("Album deleted:", albumName);
      if (selectedAlbum === albumName) {
        selectedAlbum = "Default";
      }
      loadAlbums();
      loadImagesForAlbum(selectedAlbum);
    } else {
      alert("Failed to delete album");
    }
  } catch (error) {
    console.error("Failed to delete album:", error);
  }
}

async function loadImages() {
  loadImagesForAlbum(selectedAlbum);
}

function displayImages() {
  const imageList = document.getElementById("imageList");

  imageList.innerHTML = "";

  // Add upload button as first item
  const uploadItem = document.createElement("div");
  uploadItem.className = "image-item upload-item";
  uploadItem.innerHTML = `
        <div class="upload-placeholder">
            <div class="upload-icon">
                <svg width="32" height="32" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
                    <path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"></path>
                    <polyline points="17 8 12 3 7 8"></polyline>
                    <line x1="12" y1="3" x2="12" y2="15"></line>
                </svg>
            </div>
            <div class="upload-text">Upload Image</div>
        </div>
    `;
  uploadItem.addEventListener("click", () => {
    document.getElementById("fileInput").click();
  });
  imageList.appendChild(uploadItem);

  if (currentImages.length === 0) {
    return;
  }

  currentImages.forEach((image) => {
    const item = document.createElement("div");
    item.className = "image-item";

    const thumbnail = document.createElement("img");
    thumbnail.className = "image-thumbnail";
    // Convert .bmp to .jpg for thumbnail
    const thumbnailName = image.name.replace(/\.bmp$/i, ".jpg");
    // Use album/filename format
    const thumbnailPath = `${selectedAlbum}/${thumbnailName}`;
    thumbnail.src = `${API_BASE}/api/image?name=${encodeURIComponent(thumbnailPath)}`;
    thumbnail.alt = image.name;
    thumbnail.loading = "lazy";

    const info = document.createElement("div");
    info.className = "image-info";

    const name = document.createElement("div");
    name.className = "image-name";
    name.textContent = image.name;

    info.appendChild(name);

    const deleteBtn = document.createElement("button");
    deleteBtn.className = "delete-btn";
    deleteBtn.textContent = "×";
    deleteBtn.title = "Delete image";
    deleteBtn.addEventListener("click", (e) => {
      e.stopPropagation();
      deleteImage(image.name);
    });

    item.appendChild(thumbnail);
    item.appendChild(info);
    item.appendChild(deleteBtn);

    item.addEventListener("click", () => selectImage(image.name, item));

    imageList.appendChild(item);
  });
}

function formatFileSize(bytes) {
  if (bytes < 1024) return bytes + " B";
  if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + " KB";
  return (bytes / (1024 * 1024)).toFixed(1) + " MB";
}

async function deleteImage(filename) {
  if (!confirm(`Are you sure you want to delete "${filename}"?`)) {
    return;
  }

  try {
    // Use album/filename format
    const fullPath = `${selectedAlbum}/${filename}`;
    const response = await fetch(`${API_BASE}/api/delete`, {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
      },
      body: JSON.stringify({ filename: fullPath }),
    });

    const data = await response.json();

    if (data.status === "success") {
      console.log("Image deleted:", filename);
      loadImages(); // Reload the image list
    } else {
      alert("Failed to delete image");
    }
  } catch (error) {
    console.error("Error deleting image:", error);
    alert("Error deleting image");
  }
}

let isDisplaying = false;

async function selectImage(filename, element) {
  if (isDisplaying) {
    alert("Please wait for the current display operation to complete");
    return;
  }

  // Show confirmation dialog
  const confirmed = confirm(
    `Display "${filename}" on the e-paper screen?\n\nThis will take approximately 40 seconds to update.`,
  );
  if (!confirmed) {
    return;
  }

  // Update selection
  document.querySelectorAll(".image-item").forEach((item) => {
    item.classList.remove("selected");
  });
  element.classList.add("selected");
  selectedImage = filename;

  isDisplaying = true;

  // Show loading indicator
  const displayStatus = document.getElementById("displayStatus");
  displayStatus.style.display = "block";

  try {
    // Use album/filename format
    const fullPath = `${selectedAlbum}/${filename}`;
    const response = await fetch(`${API_BASE}/api/display`, {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
      },
      body: JSON.stringify({ filename: fullPath }),
    });

    const data = await response.json();

    if (data.status === "success") {
      console.log("Image displayed:", filename);
    } else if (data.status === "busy") {
      alert("Display is currently updating, please wait");
    } else {
      alert("Failed to display image");
    }
  } catch (error) {
    console.error("Error displaying image:", error);
    alert("Error displaying image");
  } finally {
    // Hide loading indicator and re-enable display
    displayStatus.style.display = "none";
    isDisplaying = false;
  }
}

// Global state for image processing
let currentImageFile = null;
let currentImageCanvas = null;
let originalImageData = null; // Store original unprocessed image data
let currentParams = {};

document.getElementById("fileInput").addEventListener("change", async (e) => {
  const file = e.target.files[0];
  if (!file) return;

  // Store the file and show preview
  currentImageFile = file;

  // Show image processing section and preview area
  document.getElementById("imageProcessingSection").style.display = "block";
  document.getElementById("previewArea").style.display = "block";

  // Ensure controls and buttons are visible (in case they were hidden from previous upload)
  document.querySelector(".button-group").style.display = "flex";
  document.querySelector(".controls-grid").style.display = "grid";
  document.getElementById("uploadProgress").style.display = "none";

  await loadImagePreview(file);
});

async function resizeImage(file, maxWidth, maxHeight, quality) {
  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onload = (e) => {
      const img = new Image();
      img.onload = () => {
        // Load image into a temporary canvas
        const tempCanvas = document.createElement("canvas");
        tempCanvas.width = img.width;
        tempCanvas.height = img.height;
        const tempCtx = tempCanvas.getContext("2d");
        tempCtx.drawImage(img, 0, 0);

        // Determine if portrait or landscape
        const isPortrait = img.height > img.width;

        // Set target dimensions based on orientation
        const targetWidth = isPortrait ? maxHeight : maxWidth;
        const targetHeight = isPortrait ? maxWidth : maxHeight;

        // Use shared resizeImageCover function
        const resizedCanvas = resizeImageCover(
          tempCanvas,
          targetWidth,
          targetHeight,
        );

        resizedCanvas.toBlob(
          (blob) => {
            resolve(blob);
          },
          "image/jpeg",
          quality,
        );
      };
      img.onerror = reject;
      img.src = e.target.result;
    };
    reader.onerror = reject;
    reader.readAsDataURL(file);
  });
}

async function loadImagePreview(file) {
  const previewCanvas = document.getElementById("previewCanvas");
  const originalCanvas = document.getElementById("originalCanvas");
  const statusDiv = document.getElementById("uploadStatus");

  // Clear any previous status
  statusDiv.textContent = "";
  statusDiv.className = "";

  // Fetch calibrated palette from device if not already loaded
  if (!deviceCalibratedPalette) {
    try {
      const response = await fetch(`${API_BASE}/api/settings/palette`);
      if (response.ok) {
        const palette = await response.json();
        deviceCalibratedPalette = [
          [palette.black.r, palette.black.g, palette.black.b],
          [palette.white.r, palette.white.g, palette.white.b],
          [palette.yellow.r, palette.yellow.g, palette.yellow.b],
          [palette.red.r, palette.red.g, palette.red.b],
          [0, 0, 0], // Reserved
          [palette.blue.r, palette.blue.g, palette.blue.b],
          [palette.green.r, palette.green.g, palette.green.b],
        ];
      }
    } catch (error) {
      console.error("Error loading calibrated palette:", error);
    }
  }

  try {
    // Load image and resize to display size
    const img = await loadImage(file);

    // Determine if portrait or landscape
    const isPortrait = img.height > img.width;

    // Set canvas to display dimensions (800x480 or 480x800)
    const baseWidth = isPortrait ? 480 : 800;
    const baseHeight = isPortrait ? 800 : 480;

    // Get container width for responsive sizing on mobile
    const container = previewCanvas.parentElement;
    const containerWidth = container
      ? container.clientWidth
      : window.innerWidth;
    const isMobile = window.innerWidth < 768;

    // Calculate scaled dimensions maintaining aspect ratio
    let width, height;
    if (isMobile && baseWidth > containerWidth) {
      // Scale down on mobile if canvas is wider than container
      const scale = containerWidth / baseWidth;
      width = Math.floor(baseWidth * scale);
      height = Math.floor(baseHeight * scale);
    } else {
      // Keep full resolution on desktop
      width = baseWidth;
      height = baseHeight;
    }

    previewCanvas.width = width;
    previewCanvas.height = height;
    originalCanvas.width = width;
    originalCanvas.height = height;

    // Draw image with cover mode (fill and crop)
    const scaleX = width / img.width;
    const scaleY = height / img.height;
    const scale = Math.max(scaleX, scaleY);

    const scaledWidth = img.width * scale;
    const scaledHeight = img.height * scale;
    const offsetX = (width - scaledWidth) / 2;
    const offsetY = (height - scaledHeight) / 2;

    // Draw to both canvases
    const previewCtx = previewCanvas.getContext("2d", {
      willReadFrequently: true,
    });
    const originalCtx = originalCanvas.getContext("2d");

    previewCtx.drawImage(img, offsetX, offsetY, scaledWidth, scaledHeight);
    originalCtx.drawImage(img, offsetX, offsetY, scaledWidth, scaledHeight);

    // Store canvas and original image data
    currentImageCanvas = previewCanvas;
    originalImageData = previewCtx.getImageData(0, 0, width, height);

    // Initialize comparison slider
    initComparisonSlider();

    // Apply initial processing
    updatePreview();
  } catch (error) {
    console.error("Error loading preview:", error);
    statusDiv.className = "status-error";
    statusDiv.textContent = "Error loading image preview";
  }
}

function getExifOrientation(file) {
  return new Promise((resolve) => {
    const reader = new FileReader();
    reader.onload = (e) => {
      const view = new DataView(e.target.result);
      if (view.getUint16(0, false) !== 0xffd8) {
        resolve(1); // Not a JPEG, default orientation
        return;
      }
      const length = view.byteLength;
      let offset = 2;
      while (offset < length) {
        if (view.getUint16(offset + 2, false) <= 8) {
          resolve(1);
          return;
        }
        const marker = view.getUint16(offset, false);
        offset += 2;
        if (marker === 0xffe1) {
          // EXIF marker
          if (view.getUint32((offset += 2), false) !== 0x45786966) {
            resolve(1);
            return;
          }
          const little = view.getUint16((offset += 6), false) === 0x4949;
          offset += view.getUint32(offset + 4, little);
          const tags = view.getUint16(offset, little);
          offset += 2;
          for (let i = 0; i < tags; i++) {
            if (view.getUint16(offset + i * 12, little) === 0x0112) {
              resolve(view.getUint16(offset + i * 12 + 8, little));
              return;
            }
          }
        } else if ((marker & 0xff00) !== 0xff00) {
          break;
        } else {
          offset += view.getUint16(offset, false);
        }
      }
      resolve(1); // Default orientation
    };
    reader.onerror = () => resolve(1);
    reader.readAsArrayBuffer(file);
  });
}

async function loadImage(file) {
  // Check if HEIC/HEIF and convert if needed
  let fileToLoad = file;
  const isHeic =
    file.type === "image/heic" ||
    file.type === "image/heif" ||
    file.name.toLowerCase().endsWith(".heic") ||
    file.name.toLowerCase().endsWith(".heif");

  // Detect iOS/Safari which has native HEIC support
  const isIOS =
    /iPad|iPhone|iPod/.test(navigator.userAgent) ||
    (navigator.platform === "MacIntel" && navigator.maxTouchPoints > 1);
  const isSafari = /^((?!chrome|android).)*safari/i.test(navigator.userAgent);

  if (isHeic && !isIOS && !isSafari) {
    // Only convert HEIC on non-iOS/Safari browsers
    try {
      const convertedBlob = await heic2any({
        blob: file,
        toType: "image/jpeg",
        quality: 1.0,
      });
      fileToLoad = convertedBlob;
    } catch (error) {
      console.error("HEIC conversion failed:", error);
      throw new Error(
        "Failed to convert HEIC image. Please try a different format.",
      );
    }
  }

  return new Promise(async (resolve, reject) => {
    const orientation = await getExifOrientation(file);

    // Detect iOS/Safari which automatically applies EXIF orientation
    const isIOS =
      /iPad|iPhone|iPod/.test(navigator.userAgent) ||
      (navigator.platform === "MacIntel" && navigator.maxTouchPoints > 1);
    const isSafari = /^((?!chrome|android).)*safari/i.test(navigator.userAgent);
    const browserAutoRotates = isIOS || isSafari;

    const reader = new FileReader();
    reader.onload = (e) => {
      const img = new Image();
      img.onload = () => {
        // Apply EXIF orientation only if browser doesn't do it automatically
        if (orientation > 1 && !browserAutoRotates) {
          // Load image into temporary canvas
          const tempCanvas = document.createElement("canvas");
          tempCanvas.width = img.width;
          tempCanvas.height = img.height;
          const tempCtx = tempCanvas.getContext("2d");
          tempCtx.drawImage(img, 0, 0);

          // Apply EXIF orientation transformation
          const orientedCanvas = applyExifOrientation(tempCanvas, orientation);

          // Create a new image from the oriented canvas
          const orientedImg = new Image();
          orientedImg.onload = () => resolve(orientedImg);
          orientedImg.src = orientedCanvas.toDataURL();
        } else {
          resolve(img);
        }
      };
      img.onerror = reject;
      img.src = e.target.result;
    };
    reader.onerror = reject;
    reader.readAsDataURL(fileToLoad);
  });
}

function initComparisonSlider() {
  const sliderContainer = document.getElementById("sliderContainer");
  const comparisonWrapper = document.getElementById("comparisonWrapper");
  const previewCanvas = document.getElementById("previewCanvas");

  if (!sliderContainer || !comparisonWrapper || !previewCanvas) return;

  let isDragging = false;

  function updateSliderPosition(clientX) {
    const rect = comparisonWrapper.getBoundingClientRect();
    let position = ((clientX - rect.left) / rect.width) * 100;
    position = Math.max(0, Math.min(100, position));

    sliderContainer.style.left = position + "%";
    // Clip from left: at 80% position, clip 80% from left, showing 20% dithered on right
    // inset(top right bottom left)
    previewCanvas.style.clipPath = `inset(0 0 0 ${position}%)`;
  }

  // Mouse events
  sliderContainer.addEventListener("mousedown", (e) => {
    isDragging = true;
    e.preventDefault();
  });

  document.addEventListener("mousemove", (e) => {
    if (isDragging) {
      updateSliderPosition(e.clientX);
    }
  });

  document.addEventListener("mouseup", () => {
    isDragging = false;
  });

  // Touch events
  sliderContainer.addEventListener("touchstart", (e) => {
    isDragging = true;
    e.preventDefault();
  });

  document.addEventListener("touchmove", (e) => {
    if (isDragging && e.touches.length > 0) {
      updateSliderPosition(e.touches[0].clientX);
    }
  });

  document.addEventListener("touchend", () => {
    isDragging = false;
  });

  // Click on wrapper to move slider
  comparisonWrapper.addEventListener("click", (e) => {
    if (e.target !== sliderContainer && !sliderContainer.contains(e.target)) {
      updateSliderPosition(e.clientX);
    }
  });

  // Initialize at 0% (slider on left, showing full dithered preview)
  sliderContainer.style.left = "0%";
  previewCanvas.style.clipPath = "inset(0 0 0 0%)";
}

function drawCurveVisualization() {
  const canvas = document.getElementById("curveCanvas");
  const ctx = canvas.getContext("2d");
  const width = canvas.width;
  const height = canvas.height;
  const padding = 40;
  const graphWidth = width - 2 * padding;
  const graphHeight = height - 2 * padding;

  // Clear canvas
  ctx.fillStyle = "white";
  ctx.fillRect(0, 0, width, height);

  // Draw axes
  ctx.strokeStyle = "#333";
  ctx.lineWidth = 2;
  ctx.beginPath();
  ctx.moveTo(padding, padding);
  ctx.lineTo(padding, height - padding);
  ctx.lineTo(width - padding, height - padding);
  ctx.stroke();

  // Draw axis labels
  ctx.fillStyle = "#333";
  ctx.font = "12px sans-serif";
  ctx.textAlign = "center";
  ctx.fillText("Input", width / 2, height - 10);
  ctx.save();
  ctx.translate(15, height / 2);
  ctx.rotate(-Math.PI / 2);
  ctx.fillText("Output", 0, 0);
  ctx.restore();

  // Draw grid lines
  ctx.strokeStyle = "#e0e0e0";
  ctx.lineWidth = 1;
  for (let i = 0; i <= 4; i++) {
    const x = padding + (graphWidth * i) / 4;
    const y = padding + (graphHeight * i) / 4;

    // Vertical grid lines
    ctx.beginPath();
    ctx.moveTo(x, padding);
    ctx.lineTo(x, height - padding);
    ctx.stroke();

    // Horizontal grid lines
    ctx.beginPath();
    ctx.moveTo(padding, y);
    ctx.lineTo(width - padding, y);
    ctx.stroke();
  }

  // Draw linear reference (y=x)
  ctx.strokeStyle = "#ccc";
  ctx.lineWidth = 1;
  ctx.setLineDash([5, 5]);
  ctx.beginPath();
  ctx.moveTo(padding, height - padding);
  ctx.lineTo(width - padding, padding);
  ctx.stroke();
  ctx.setLineDash([]);

  // Draw S-curve
  ctx.strokeStyle = "#667eea";
  ctx.lineWidth = 3;
  ctx.beginPath();

  const { strength, shadowBoost, highlightCompress, midpoint } = currentParams;

  for (let i = 0; i <= graphWidth; i++) {
    const normalized = i / graphWidth;
    let result;

    if (normalized <= midpoint) {
      // Shadow region
      const shadowVal = normalized / midpoint;
      result = Math.pow(shadowVal, 1.0 - strength * shadowBoost) * midpoint;
    } else {
      // Highlight region
      const highlightVal = (normalized - midpoint) / (1.0 - midpoint);
      result =
        midpoint +
        Math.pow(highlightVal, 1.0 + strength * highlightCompress) *
          (1.0 - midpoint);
    }

    const x = padding + i;
    const y = height - padding - result * graphHeight;

    if (i === 0) {
      ctx.moveTo(x, y);
    } else {
      ctx.lineTo(x, y);
    }
  }

  ctx.stroke();

  // Draw midpoint indicator
  ctx.strokeStyle = "#ff6b6b";
  ctx.lineWidth = 2;
  ctx.setLineDash([3, 3]);
  const midX = padding + midpoint * graphWidth;
  ctx.beginPath();
  ctx.moveTo(midX, padding);
  ctx.lineTo(midX, height - padding);
  ctx.stroke();
  ctx.setLineDash([]);

  // Midpoint label
  ctx.fillStyle = "#ff6b6b";
  ctx.font = "11px sans-serif";
  ctx.textAlign = "center";
  ctx.fillText("midpoint", midX, padding - 5);
}

function updatePreview() {
  if (!currentImageCanvas || !originalImageData) return;

  // Update curve visualization
  drawCurveVisualization();

  // Create a copy of the original image data
  const tempCanvas = document.createElement("canvas");
  tempCanvas.width = originalImageData.width;
  tempCanvas.height = originalImageData.height;
  const tempCtx = tempCanvas.getContext("2d", { willReadFrequently: true });

  // Copy original image data
  const imageDataCopy = tempCtx.createImageData(
    originalImageData.width,
    originalImageData.height,
  );
  imageDataCopy.data.set(originalImageData.data);

  // Apply image processing with device calibrated palette for accurate preview
  const previewParams = { ...currentParams };
  if (deviceCalibratedPalette) {
    previewParams.customPalette = deviceCalibratedPalette;
  }
  processImage(imageDataCopy, previewParams);

  // Put processed data back to temp canvas
  tempCtx.putImageData(imageDataCopy, 0, 0);

  // Draw processed image to preview canvas
  const previewCanvas = document.getElementById("previewCanvas");
  const ctx = previewCanvas.getContext("2d");
  ctx.drawImage(tempCanvas, 0, 0);
}

// Parameter change handlers
document.getElementById("exposure").addEventListener("input", (e) => {
  currentParams.exposure = parseFloat(e.target.value);
  document.getElementById("exposureValue").textContent = e.target.value;
  updatePreview();
  scheduleSaveSettings();
});

document.getElementById("saturation").addEventListener("input", (e) => {
  currentParams.saturation = parseFloat(e.target.value);
  document.getElementById("saturationValue").textContent = e.target.value;
  updatePreview();
  scheduleSaveSettings();
});

document.getElementById("contrast").addEventListener("input", (e) => {
  currentParams.contrast = parseFloat(e.target.value);
  document.getElementById("contrastValue").textContent = e.target.value;
  updatePreview();
  scheduleSaveSettings();
});

document.getElementById("scurveStrength").addEventListener("input", (e) => {
  currentParams.strength = parseFloat(e.target.value);
  document.getElementById("strengthValue").textContent = e.target.value;
  updatePreview();
  scheduleSaveSettings();
});

document.getElementById("scurveShadow").addEventListener("input", (e) => {
  currentParams.shadowBoost = parseFloat(e.target.value);
  document.getElementById("shadowValue").textContent = e.target.value;
  updatePreview();
  scheduleSaveSettings();
});

document.getElementById("scurveHighlight").addEventListener("input", (e) => {
  currentParams.highlightCompress = parseFloat(e.target.value);
  document.getElementById("highlightValue").textContent = e.target.value;
  updatePreview();
  scheduleSaveSettings();
});

document.getElementById("scurveMidpoint").addEventListener("input", (e) => {
  currentParams.midpoint = parseFloat(e.target.value);
  document.getElementById("midpointValue").textContent = e.target.value;
  updatePreview();
  scheduleSaveSettings();
});

// Color matching method radio buttons
document.querySelectorAll('input[name="colorMethod"]').forEach((radio) => {
  radio.addEventListener("change", (e) => {
    currentParams.colorMethod = e.target.value;
    updatePreview();
    scheduleSaveSettings();
  });
});

// Tone mode radio buttons
document.querySelectorAll('input[name="toneMode"]').forEach((radio) => {
  radio.addEventListener("change", (e) => {
    currentParams.toneMode = e.target.value;

    // Show/hide contrast or S-curve controls based on mode
    const contrastControl = document.getElementById("contrastControl");
    const curveCanvasWrapper = document.querySelector(".curve-canvas-wrapper");

    if (e.target.value === "contrast") {
      contrastControl.style.display = "block";
      curveCanvasWrapper.style.display = "none";
    } else {
      contrastControl.style.display = "none";
      curveCanvasWrapper.style.display = "flex";
    }

    updatePreview();
    scheduleSaveSettings();
  });
});

// Processing mode radio buttons
document.querySelectorAll('input[name="processingMode"]').forEach((radio) => {
  radio.addEventListener("change", (e) => {
    currentParams.processingMode = e.target.value;

    // Show/hide enhanced controls and canvas based on mode
    const enhancedControls = document.getElementById("enhancedControls");
    const colorMethodControl = document.getElementById("colorMethodControl");
    const curveCanvasWrapper = document.querySelector(".curve-canvas-wrapper");

    if (e.target.value === "stock") {
      enhancedControls.style.display = "none";
      colorMethodControl.style.display = "none";
      curveCanvasWrapper.style.display = "none";
    } else {
      enhancedControls.style.display = "grid";
      colorMethodControl.style.display = "block";
      // Show curve canvas only if S-curve mode is selected
      if (currentParams.toneMode === "scurve") {
        curveCanvasWrapper.style.display = "block";
      }
    }

    updatePreview();
    scheduleSaveSettings();
  });
});

// Discard image and return to upload
document.getElementById("discardImage").addEventListener("click", () => {
  currentImageFile = null;
  currentImageCanvas = null;
  originalImageData = null;
  document.getElementById("fileInput").value = "";
  document.getElementById("imageProcessingSection").style.display = "none";
  document.getElementById("previewArea").style.display = "none";
  document.getElementById("uploadStatus").textContent = "";
  document.getElementById("uploadStatus").className = "";
});

// Reset to defaults - fetch from firmware backend
document.getElementById("resetParams").addEventListener("click", async () => {
  try {
    // Reset to firmware defaults using DELETE method
    const response = await fetch(`${API_BASE}/api/settings/processing`, {
      method: "DELETE",
    });
    if (response.ok) {
      const defaults = await response.json();

      // Update currentParams
      currentParams = { ...defaults };

      // Update UI
      document.getElementById("exposure").value = defaults.exposure;
      document.getElementById("exposureValue").textContent =
        defaults.exposure.toFixed(1);
      document.getElementById("saturation").value = defaults.saturation;
      document.getElementById("saturationValue").textContent =
        defaults.saturation.toFixed(1);
      document.getElementById("contrast").value = defaults.contrast;
      document.getElementById("contrastValue").textContent =
        defaults.contrast.toFixed(1);
      document.getElementById("scurveStrength").value = defaults.strength;
      document.getElementById("strengthValue").textContent =
        defaults.strength.toFixed(1);
      document.getElementById("scurveShadow").value = defaults.shadowBoost;
      document.getElementById("shadowValue").textContent =
        defaults.shadowBoost.toFixed(1);
      document.getElementById("scurveHighlight").value =
        defaults.highlightCompress;
      document.getElementById("highlightValue").textContent =
        defaults.highlightCompress.toFixed(1);
      document.getElementById("scurveMidpoint").value = defaults.midpoint;
      document.getElementById("midpointValue").textContent =
        defaults.midpoint.toFixed(1);
      document.querySelector(
        `input[name="colorMethod"][value="${defaults.colorMethod}"]`,
      ).checked = true;
      document.querySelector(
        `input[name="toneMode"][value="${defaults.toneMode}"]`,
      ).checked = true;
      document.querySelector(
        `input[name="processingMode"][value="${defaults.processingMode}"]`,
      ).checked = true;

      // Update UI visibility
      const enhancedControls = document.getElementById("enhancedControls");
      const colorMethodControl = document.getElementById("colorMethodControl");
      const contrastControl = document.getElementById("contrastControl");
      const curveCanvasWrapper = document.querySelector(
        ".curve-canvas-wrapper",
      );

      if (defaults.processingMode === "stock") {
        enhancedControls.style.display = "none";
        colorMethodControl.style.display = "none";
        curveCanvasWrapper.style.display = "none";
      } else {
        enhancedControls.style.display = "grid";
        colorMethodControl.style.display = "block";
        if (defaults.toneMode === "scurve") {
          contrastControl.style.display = "none";
          curveCanvasWrapper.style.display = "flex";
        } else {
          contrastControl.style.display = "block";
          curveCanvasWrapper.style.display = "none";
        }
      }

      updatePreview();
      scheduleSaveSettings();
    } else {
      console.error("Failed to fetch defaults from firmware");
    }
  } catch (error) {
    console.error("Error resetting to defaults:", error);
  }
});

// Upload processed image
document
  .getElementById("uploadProcessed")
  .addEventListener("click", async () => {
    if (!currentImageFile || !currentImageCanvas) {
      alert("No image loaded");
      return;
    }

    const statusDiv = document.getElementById("uploadStatus");
    const uploadProgress = document.getElementById("uploadProgress");
    const buttonGroup = document.querySelector(".button-group");
    const controlsGrid = document.querySelector(".controls-grid");

    // Hide buttons and controls, show progress
    buttonGroup.style.display = "none";
    controlsGrid.style.display = "none";
    uploadProgress.style.display = "block";
    statusDiv.textContent = "";
    statusDiv.className = "";

    // Wait for UI to update
    await new Promise((resolve) => setTimeout(resolve, 0));

    let uploadSucceeded = false;

    try {
      let imageBlob;

      if (currentParams.processingMode === "stock") {
        // Stock mode: send raw scaled/cropped image (no processing)
        imageBlob = await resizeImage(currentImageFile, 800, 480, 0.9);
      } else {
        // Enhanced mode: apply exposure, saturation, and tone mapping, but NO dithering
        // Create a temporary canvas for processing
        const tempCanvas = document.createElement("canvas");
        tempCanvas.width = originalImageData.width;
        tempCanvas.height = originalImageData.height;
        const tempCtx = tempCanvas.getContext("2d", {
          willReadFrequently: true,
        });

        // Copy original image data
        const imageDataCopy = tempCtx.createImageData(
          originalImageData.width,
          originalImageData.height,
        );
        imageDataCopy.data.set(originalImageData.data);

        // Apply exposure
        if (currentParams.exposure && currentParams.exposure !== 1.0) {
          applyExposure(imageDataCopy, currentParams.exposure);
        }

        // Apply saturation
        if (currentParams.saturation !== 1.0) {
          applySaturation(imageDataCopy, currentParams.saturation);
        }

        // Apply tone mapping (contrast or S-curve)
        if (currentParams.toneMode === "contrast") {
          if (currentParams.contrast && currentParams.contrast !== 1.0) {
            applyContrast(imageDataCopy, currentParams.contrast);
          }
        } else {
          // S-curve tone mapping
          applyScurveTonemap(
            imageDataCopy,
            currentParams.strength,
            currentParams.shadowBoost,
            currentParams.highlightCompress,
            currentParams.midpoint,
          );
        }

        // Put processed data to canvas (NO dithering)
        tempCtx.putImageData(imageDataCopy, 0, 0);

        // Convert to blob
        imageBlob = await new Promise((resolve) => {
          tempCanvas.toBlob(resolve, "image/jpeg", 0.9);
        });
      }

      // Create thumbnail (320x192 or 192x320) from original using shared function
      const thumbnailBlob = await new Promise((resolve, reject) => {
        const reader = new FileReader();
        reader.onload = (e) => {
          const img = new Image();
          img.onload = () => {
            // Load image into a canvas first
            const tempCanvas = document.createElement("canvas");
            tempCanvas.width = img.width;
            tempCanvas.height = img.height;
            const tempCtx = tempCanvas.getContext("2d");
            tempCtx.drawImage(img, 0, 0);

            // Generate thumbnail using shared function
            const thumbCanvas = generateThumbnail(tempCanvas, 400, 240);

            // Convert to blob
            thumbCanvas.toBlob(resolve, "image/jpeg", 0.85);
          };
          img.onerror = reject;
          img.src = e.target.result;
        };
        reader.onerror = reject;
        reader.readAsDataURL(currentImageFile);
      });

      const formData = new FormData();
      // Send text fields first so backend can parse them before processing files
      formData.append("album", selectedAlbum);
      formData.append("processingMode", currentParams.processingMode);
      formData.append("image", imageBlob, currentImageFile.name);
      formData.append(
        "thumbnail",
        thumbnailBlob,
        "thumb_" + currentImageFile.name,
      );

      const response = await fetch(`${API_BASE}/api/upload`, {
        method: "POST",
        body: formData,
      });

      const data = await response.json();

      if (data.status === "success") {
        uploadSucceeded = true;

        // Reset state
        currentImageFile = null;
        currentImageCanvas = null;
        originalImageData = null;
        document.getElementById("fileInput").value = "";

        // Hide processing section
        document.getElementById("imageProcessingSection").style.display =
          "none";
        document.getElementById("previewArea").style.display = "none";
        uploadProgress.style.display = "none";

        // Refresh image gallery
        loadImages();
      } else {
        statusDiv.className = "status-error";
        statusDiv.textContent = "Upload failed";
      }
    } catch (error) {
      console.error("Error uploading:", error);
      statusDiv.className = "status-error";
      statusDiv.textContent = "Error uploading file";
    } finally {
      // Only show controls if upload failed (still on preview)
      if (!uploadSucceeded) {
        buttonGroup.style.display = "flex";
        controlsGrid.style.display = "grid";
        uploadProgress.style.display = "none";
      }
    }
  });

async function loadConfig() {
  try {
    const response = await fetch(`${API_BASE}/api/config`);
    if (
      !response.ok ||
      response.headers.get("content-type")?.includes("text/html")
    ) {
      // Running in standalone mode without ESP32 backend
      console.log("Config API not available (standalone mode)");
      return;
    }
    const data = await response.json();

    document.getElementById("autoRotate").checked = data.auto_rotate || false;
    document.getElementById("rotateInterval").value =
      data.rotate_interval || 3600;
    document.getElementById("imageOrientation").value =
      data.image_orientation || 180;
    document.getElementById("imageUrl").value =
      data.image_url || "https://picsum.photos/800/480";
    document.getElementById("haUrl").value = data.ha_url || "";
    document.getElementById("deepSleepEnabled").checked =
      data.deep_sleep_enabled !== false;
    document.getElementById("saveDownloadedImages").checked =
      data.save_downloaded_images !== false;

    // Set rotation mode based on backend config
    const rotationMode = data.rotation_mode || "sdcard";
    if (rotationMode === "url") {
      document.getElementById("rotationModeURL").checked = true;
      document.getElementById("imageUrlGroup").style.display = "block";
    } else {
      document.getElementById("rotationModeSDCard").checked = true;
      document.getElementById("imageUrlGroup").style.display = "none";
    }

    // Update warning visibility based on loaded state
    updateDeepSleepWarning();
  } catch (error) {
    // Silently fail if API not available (standalone mode)
    console.log("Config API not available (standalone mode)");
  }
}

// Toggle deep sleep warning visibility based on checkbox state
function updateDeepSleepWarning() {
  const deepSleepEnabled = document.getElementById("deepSleepEnabled").checked;
  const warning = document.getElementById("deepSleepWarning");
  warning.style.display = deepSleepEnabled ? "none" : "block";
}

// Add event listener to deep sleep checkbox
document
  .getElementById("deepSleepEnabled")
  .addEventListener("change", updateDeepSleepWarning);

// Add event listeners for rotation mode radio buttons
document
  .getElementById("rotationModeSDCard")
  .addEventListener("change", function () {
    document.getElementById("imageUrlGroup").style.display = "none";
  });

document
  .getElementById("rotationModeURL")
  .addEventListener("change", function () {
    document.getElementById("imageUrlGroup").style.display = "block";
  });

document.getElementById("configForm").addEventListener("submit", async (e) => {
  e.preventDefault();

  const statusDiv = document.getElementById("configStatus");
  const autoRotate = document.getElementById("autoRotate").checked;
  const rotateInterval = parseInt(
    document.getElementById("rotateInterval").value,
  );
  const imageOrientation = parseInt(
    document.getElementById("imageOrientation").value,
  );
  const rotationMode = document.querySelector(
    'input[name="rotationMode"]:checked',
  ).value;
  const imageUrl = document.getElementById("imageUrl").value;
  const haUrl = document.getElementById("haUrl").value;
  const deepSleepEnabled = document.getElementById("deepSleepEnabled").checked;
  const saveDownloadedImages = document.getElementById(
    "saveDownloadedImages",
  ).checked;

  try {
    const response = await fetch(`${API_BASE}/api/config`, {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
      },
      body: JSON.stringify({
        auto_rotate: autoRotate,
        rotate_interval: rotateInterval,
        image_orientation: imageOrientation,
        rotation_mode: rotationMode,
        image_url: imageUrl,
        ha_url: haUrl,
        deep_sleep_enabled: deepSleepEnabled,
        save_downloaded_images: saveDownloadedImages,
      }),
    });

    const data = await response.json();

    if (data.status === "success") {
      statusDiv.className = "status-success";
      statusDiv.textContent = "Settings saved successfully";
    } else {
      statusDiv.className = "status-error";
      statusDiv.textContent = "Failed to save settings";
    }
  } catch (error) {
    console.error("Error saving config:", error);
    statusDiv.className = "status-error";
    statusDiv.textContent = "Error saving settings";
  }
});

// Periodic updates - only when page is visible/focused
let imageInterval = null;
let batteryInterval = null;

function startPeriodicUpdates() {
  // Only start if not already running
  if (!imageInterval) {
    imageInterval = setInterval(loadImages, 30000);
  }
  if (!batteryInterval) {
    batteryInterval = setInterval(loadBatteryStatus, 60000);
  }
}

function stopPeriodicUpdates() {
  if (imageInterval) {
    clearInterval(imageInterval);
    imageInterval = null;
  }
  if (batteryInterval) {
    clearInterval(batteryInterval);
    batteryInterval = null;
  }
}

// Load version information from API
async function loadVersion() {
  try {
    const response = await fetch(`${API_BASE}/api/version`);
    if (
      !response.ok ||
      response.headers.get("content-type")?.includes("text/html")
    ) {
      // Running in standalone mode without ESP32 backend
      console.log("Version API not available (standalone mode)");
      return;
    }
    const data = await response.json();

    // Update footer with version
    const footer = document.querySelector("footer p");
    if (footer && data.version) {
      footer.textContent = `ESP32-S3 PhotoFrame ${data.version}`;
    }
  } catch (error) {
    // Silently fail if API not available (standalone mode)
    console.log("Version API not available (standalone mode)");
  }
}

// Listen for page visibility changes
document.addEventListener("visibilitychange", () => {
  if (document.hidden) {
    console.log("Page hidden - stopping periodic updates");
    stopPeriodicUpdates();
  } else {
    console.log("Page visible - starting periodic updates");
    // Refresh data immediately when page becomes visible
    loadAlbums();
    loadImages();
    loadBatteryStatus();
    startPeriodicUpdates();
  }
});

// Start periodic updates if page is currently visible
if (!document.hidden) {
  startPeriodicUpdates();
}
// Initial load
loadAlbums();
loadImages();
loadConfig();
loadVersion();

// Expose createAlbum to global scope for button onclick
window.createAlbum = createAlbum;
loadBatteryStatus();

// Setup drag & drop for gallery area
function setupDragAndDrop() {
  const imageList = document.getElementById("imageList");
  if (!imageList) return;

  ["dragenter", "dragover", "dragleave", "drop"].forEach((eventName) => {
    imageList.addEventListener(eventName, preventDefaults, false);
  });

  function preventDefaults(e) {
    e.preventDefault();
    e.stopPropagation();
  }

  ["dragenter", "dragover"].forEach((eventName) => {
    imageList.addEventListener(
      eventName,
      () => {
        imageList.classList.add("drag-over");
      },
      false,
    );
  });

  ["dragleave", "drop"].forEach((eventName) => {
    imageList.addEventListener(
      eventName,
      () => {
        imageList.classList.remove("drag-over");
      },
      false,
    );
  });

  imageList.addEventListener("drop", handleDrop, false);

  function handleDrop(e) {
    const dt = e.dataTransfer;
    const files = dt.files;

    if (files.length > 0) {
      const file = files[0];
      if (file.type === "image/jpeg" || file.type === "image/jpg") {
        const fileInput = document.getElementById("fileInput");
        const dataTransfer = new DataTransfer();
        dataTransfer.items.add(file);
        fileInput.files = dataTransfer.files;

        // Trigger file input change event
        const event = new Event("change", { bubbles: true });
        fileInput.dispatchEvent(event);
      } else {
        alert("Please upload a JPG/JPEG image");
      }
    }
  }
}

// Save settings to backend with debouncing
function scheduleSaveSettings() {
  if (settingsSaveTimer) {
    clearTimeout(settingsSaveTimer);
  }

  settingsSaveTimer = setTimeout(async () => {
    try {
      const response = await fetch(`${API_BASE}/api/settings/processing`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(currentParams),
      });

      if (response.ok) {
        console.log("Settings saved to device");
      } else {
        console.error("Failed to save settings");
      }
    } catch (error) {
      console.error("Error saving settings:", error);
    }
  }, SETTINGS_SAVE_DELAY);
}

// Load persisted settings from backend
async function loadPersistedSettings() {
  try {
    const response = await fetch(`${API_BASE}/api/settings/processing`);
    if (response.ok) {
      const settings = await response.json();

      // Update currentParams with persisted values
      currentParams = { ...settings };

      // Update UI to reflect loaded settings
      document.getElementById("exposure").value = settings.exposure;
      document.getElementById("exposureValue").textContent =
        settings.exposure.toFixed(1);
      document.getElementById("saturation").value = settings.saturation;
      document.getElementById("saturationValue").textContent =
        settings.saturation.toFixed(1);
      document.getElementById("contrast").value = settings.contrast;
      document.getElementById("contrastValue").textContent =
        settings.contrast.toFixed(1);
      document.getElementById("scurveStrength").value = settings.strength;
      document.getElementById("strengthValue").textContent =
        settings.strength.toFixed(1);
      document.getElementById("scurveShadow").value = settings.shadowBoost;
      document.getElementById("shadowValue").textContent =
        settings.shadowBoost.toFixed(1);
      document.getElementById("scurveHighlight").value =
        settings.highlightCompress;
      document.getElementById("highlightValue").textContent =
        settings.highlightCompress.toFixed(1);
      document.getElementById("scurveMidpoint").value = settings.midpoint;
      document.getElementById("midpointValue").textContent =
        settings.midpoint.toFixed(1);
      document.querySelector(
        `input[name="colorMethod"][value="${settings.colorMethod}"]`,
      ).checked = true;
      document.querySelector(
        `input[name="toneMode"][value="${settings.toneMode}"]`,
      ).checked = true;
      document.querySelector(
        `input[name="processingMode"][value="${settings.processingMode}"]`,
      ).checked = true;

      // Update UI visibility based on loaded settings
      const enhancedControls = document.getElementById("enhancedControls");
      const colorMethodControl = document.getElementById("colorMethodControl");
      const contrastControl = document.getElementById("contrastControl");
      const curveCanvasWrapper = document.querySelector(
        ".curve-canvas-wrapper",
      );

      if (settings.processingMode === "stock") {
        enhancedControls.style.display = "none";
        colorMethodControl.style.display = "none";
        curveCanvasWrapper.style.display = "none";
      } else {
        enhancedControls.style.display = "grid";
        colorMethodControl.style.display = "block";
        if (settings.toneMode === "scurve") {
          contrastControl.style.display = "none";
          curveCanvasWrapper.style.display = "flex";
        } else {
          contrastControl.style.display = "block";
          curveCanvasWrapper.style.display = "none";
        }
      }

      console.log("Loaded persisted settings from device");
    } else {
      console.error("Failed to load settings from firmware");
    }
  } catch (error) {
    console.error("Error loading settings:", error);
  }
}

setupDragAndDrop();
loadPersistedSettings();

// ===== Color Palette Calibration =====

let measuredPaletteData = null;

// Load and display current palette
let currentPaletteData = null;
let originalPaletteData = null;
let deviceCalibratedPalette = null; // Palette for preview dithering

async function loadColorPalette() {
  try {
    const response = await fetch(`${API_BASE}/api/settings/palette`);
    if (response.ok) {
      const palette = await response.json();
      currentPaletteData = JSON.parse(JSON.stringify(palette));
      originalPaletteData = JSON.parse(JSON.stringify(palette));

      // Convert palette to array format for preview dithering
      deviceCalibratedPalette = [
        [palette.black.r, palette.black.g, palette.black.b],
        [palette.white.r, palette.white.g, palette.white.b],
        [palette.yellow.r, palette.yellow.g, palette.yellow.b],
        [palette.red.r, palette.red.g, palette.red.b],
        [0, 0, 0], // Reserved
        [palette.blue.r, palette.blue.g, palette.blue.b],
        [palette.green.r, palette.green.g, palette.green.b],
      ];

      displayPalette(palette);

      // Update preview if image is loaded
      if (currentImageCanvas && originalImageData) {
        updatePreview();
      }
    }
  } catch (error) {
    console.error("Error loading palette:", error);
  }
}

function displayPalette(palette) {
  const container = document.getElementById("currentPaletteColors");
  container.innerHTML = "";

  const colors = ["black", "white", "yellow", "red", "blue", "green"];
  colors.forEach((color) => {
    const rgb = palette[color];
    const div = document.createElement("div");
    div.className = "palette-color";
    div.innerHTML = `
            <div class="palette-swatch" id="current-swatch-${color}" style="background-color: rgb(${rgb.r}, ${rgb.g}, ${rgb.b});"></div>
            <div class="palette-label">${color.charAt(0).toUpperCase() + color.slice(1)}</div>
            <div class="palette-rgb-inputs" style="display: flex; gap: 4px; margin-top: 5px;">
                <input type="number" id="current-r-${color}" min="0" max="255" value="${rgb.r}" 
                       style="width: 50px; padding: 2px; text-align: center;" 
                       data-color="${color}" data-channel="r" class="current-rgb-input">
                <input type="number" id="current-g-${color}" min="0" max="255" value="${rgb.g}" 
                       style="width: 50px; padding: 2px; text-align: center;" 
                       data-color="${color}" data-channel="g" class="current-rgb-input">
                <input type="number" id="current-b-${color}" min="0" max="255" value="${rgb.b}" 
                       style="width: 50px; padding: 2px; text-align: center;" 
                       data-color="${color}" data-channel="b" class="current-rgb-input">
            </div>
        `;
    container.appendChild(div);
  });

  // Add event listeners
  document.querySelectorAll(".current-rgb-input").forEach((input) => {
    input.addEventListener("input", (e) => {
      const color = e.target.dataset.color;
      const channel = e.target.dataset.channel;
      let value = parseInt(e.target.value);

      if (isNaN(value)) value = 0;
      if (value < 0) value = 0;
      if (value > 255) value = 255;
      e.target.value = value;

      currentPaletteData[color][channel] = value;

      const swatch = document.getElementById(`current-swatch-${color}`);
      const r = currentPaletteData[color].r;
      const g = currentPaletteData[color].g;
      const b = currentPaletteData[color].b;
      swatch.style.backgroundColor = `rgb(${r}, ${g}, ${b})`;

      checkPaletteChanges();
    });
  });

  document.getElementById("savePaletteButtonContainer").style.display = "none";
}

function checkPaletteChanges() {
  const hasChanges =
    JSON.stringify(currentPaletteData) !== JSON.stringify(originalPaletteData);
  document.getElementById("savePaletteButtonContainer").style.display =
    hasChanges ? "block" : "none";
}

// Start calibration flow
document.getElementById("startCalibrationBtn").addEventListener("click", () => {
  // Clear all previous calibration state
  measuredPaletteData = null;

  // Clear all status messages
  document.getElementById("displayCalibrationStatus").innerHTML = "";
  document.getElementById("calibrationAnalysisStatus").innerHTML = "";
  document.getElementById("saveCalibrationStatus").innerHTML = "";

  // Clear canvas and file input
  const canvas = document.getElementById("calibrationCanvas");
  canvas.style.display = "none";
  const ctx = canvas.getContext("2d");
  ctx.clearRect(0, 0, canvas.width, canvas.height);
  document.getElementById("calibrationPhotoInput").value = "";

  // Clear measured palette display
  document.getElementById("measuredPalette").innerHTML = "";

  // Show calibration flow and reset to step 1
  document.getElementById("calibrationFlow").style.display = "block";
  document.getElementById("startCalibrationBtn").style.display = "none";
  document.getElementById("calibrationStep1").style.display = "block";
  document.getElementById("calibrationStep2").style.display = "none";
  document.getElementById("calibrationStep3").style.display = "none";
  document.getElementById("calibrationStep4").style.display = "none";
});

// Skip display step if pattern is already shown
document.getElementById("skipDisplayBtn").addEventListener("click", () => {
  document.getElementById("calibrationStep1").style.display = "none";
  document.getElementById("calibrationStep2").style.display = "block";
});

// Display calibration pattern on device
document
  .getElementById("displayCalibrationBtn")
  .addEventListener("click", async () => {
    const statusDiv = document.getElementById("displayCalibrationStatus");
    statusDiv.textContent = "Displaying calibration pattern on device...";
    statusDiv.className = "status-info";

    try {
      // Call API to display calibration pattern directly on the e-paper
      const response = await fetch(`${API_BASE}/api/calibration/display`, {
        method: "POST",
      });

      if (response.ok) {
        const result = await response.json();
        statusDiv.textContent = "Calibration pattern displayed on device";
        statusDiv.className = "status-success";

        // Move to next step
        setTimeout(() => {
          document.getElementById("calibrationStep1").style.display = "none";
          document.getElementById("calibrationStep2").style.display = "block";
        }, 1500);
      } else {
        const error = await response.json();
        throw new Error(
          error.message || "Failed to display calibration pattern",
        );
      }
    } catch (error) {
      statusDiv.textContent = `Error: ${error.message}`;
      statusDiv.className = "status-error";
    }
  });

// Proceed to upload step
document.getElementById("proceedToUploadBtn").addEventListener("click", () => {
  document.getElementById("calibrationStep2").style.display = "none";
  document.getElementById("calibrationStep3").style.display = "block";
});

// Analyze uploaded photo
document
  .getElementById("calibrationPhotoInput")
  .addEventListener("change", async (e) => {
    const file = e.target.files[0];
    if (!file) return;

    const statusDiv = document.getElementById("calibrationAnalysisStatus");
    statusDiv.textContent = "Analyzing photo...";
    statusDiv.className = "status-info";

    try {
      const img = await loadImage(file);
      const canvas = document.getElementById("calibrationCanvas");
      canvas.width = img.width;
      canvas.height = img.height;
      const ctx = canvas.getContext("2d");
      ctx.drawImage(img, 0, 0);
      canvas.style.display = "block";

      // Extract color boxes
      const palette = extractColorBoxes(ctx, img.width, img.height);

      if (palette) {
        // Validate extracted colors
        const validation = validateExtractedColors(palette);

        if (!validation.valid) {
          throw new Error(validation.message);
        }

        measuredPaletteData = palette;
        statusDiv.textContent = "Colors extracted successfully";
        statusDiv.className = "status-success";

        // Move to review step
        setTimeout(() => {
          document.getElementById("calibrationStep3").style.display = "none";
          document.getElementById("calibrationStep4").style.display = "block";
          displayMeasuredPalette(palette);
        }, 1000);
      } else {
        throw new Error(
          "Could not detect color boxes. Please ensure the photo shows the entire display clearly.",
        );
      }
    } catch (error) {
      const errorMsg =
        error.message || error.toString() || "Unknown error occurred";
      statusDiv.textContent = `Error: ${errorMsg}`;
      statusDiv.className = "status-error";
      console.error("Calibration photo analysis error:", error);
    }
  });

function validateExtractedColors(palette) {
  // Expected color characteristics for E6 display
  const expectations = {
    black: { maxBrightness: 50, name: "Black" },
    white: { minBrightness: 150, name: "White" },
    yellow: { minR: 150, minG: 150, maxB: 100, name: "Yellow" },
    red: { minR: 100, maxG: 80, maxB: 80, name: "Red" },
    blue: { maxR: 100, maxG: 100, minB: 100, name: "Blue" },
    green: { maxR: 100, minG: 80, maxB: 100, name: "Green" },
  };

  const errors = [];

  // Helper to calculate brightness
  const brightness = (color) => (color.r + color.g + color.b) / 3;

  // Validate black - should be dark
  if (brightness(palette.black) > expectations.black.maxBrightness) {
    errors.push(
      `Black is too bright (${Math.round(brightness(palette.black))}). Expected < ${expectations.black.maxBrightness}.`,
    );
  }

  // Validate white - should be bright
  if (brightness(palette.white) < expectations.white.minBrightness) {
    errors.push(
      `White is too dark (${Math.round(brightness(palette.white))}). Expected > ${expectations.white.minBrightness}.`,
    );
  }

  // Validate yellow - should have high R and G, low B
  if (
    palette.yellow.r < expectations.yellow.minR ||
    palette.yellow.g < expectations.yellow.minG
  ) {
    errors.push(
      `Yellow doesn't have enough red/green. Check lighting and ensure no color cast.`,
    );
  }
  if (palette.yellow.b > expectations.yellow.maxB) {
    errors.push(`Yellow has too much blue. Photo may have a blue color cast.`);
  }

  // Validate red - should have high R, low G and B
  if (palette.red.r < expectations.red.minR) {
    errors.push(`Red is not red enough. Check lighting conditions.`);
  }
  if (
    palette.red.g > expectations.red.maxG ||
    palette.red.b > expectations.red.maxB
  ) {
    errors.push(`Red has too much green/blue contamination.`);
  }

  // Validate blue - should have high B, low R and G
  if (palette.blue.b < expectations.blue.minB) {
    errors.push(`Blue is not blue enough. Check lighting conditions.`);
  }
  if (
    palette.blue.r > expectations.blue.maxR ||
    palette.blue.g > expectations.blue.maxG
  ) {
    errors.push(`Blue has too much red/green contamination.`);
  }

  // Validate green - should have high G, low R and B
  if (palette.green.g < expectations.green.minG) {
    errors.push(`Green is not green enough. Check lighting conditions.`);
  }
  if (
    palette.green.r > expectations.green.maxR ||
    palette.green.b > expectations.green.maxB
  ) {
    errors.push(`Green has too much red/blue contamination.`);
  }

  // Check for overall issues
  const avgBrightness =
    (brightness(palette.black) +
      brightness(palette.white) +
      brightness(palette.yellow) +
      brightness(palette.red) +
      brightness(palette.blue) +
      brightness(palette.green)) /
    6;

  if (avgBrightness < 50) {
    errors.push(
      "Photo is too dark overall. Use better lighting (5500K daylight).",
    );
  }

  if (avgBrightness > 200) {
    errors.push(
      "Photo is overexposed. Reduce exposure or move away from direct light.",
    );
  }

  if (errors.length > 0) {
    return {
      valid: false,
      message:
        "Color validation failed:\n• " +
        errors.join("\n• ") +
        "\n\nPlease retake the photo under proper lighting (5500K daylight, no shadows/glare). Refer to the example image.",
    };
  }

  return { valid: true };
}

function extractColorBoxes(ctx, width, height) {
  // More robust algorithm: detect color boxes by finding regions with distinct colors
  // Strategy: scan image to find 6 largest uniform color regions

  const imageData = ctx.getImageData(0, 0, width, height);
  const data = imageData.data;

  // Step 1: Downsample image for faster processing (every 4th pixel)
  const step = 4;
  const samples = [];

  for (let y = 0; y < height; y += step) {
    for (let x = 0; x < width; x += step) {
      const idx = (y * width + x) * 4;
      samples.push({
        x: x,
        y: y,
        r: data[idx],
        g: data[idx + 1],
        b: data[idx + 2],
      });
    }
  }

  // Step 2: Cluster samples into color groups using simple k-means-like approach
  // We expect 6 main colors + white background
  const colorGroups = clusterColors(samples, 7);

  // Step 3: Identify the 6 color boxes (exclude white background)
  // Sort by area (largest first) and take top 6 non-white groups
  const boxes = colorGroups
    .filter((group) => {
      // Filter out white/light background (high brightness, low saturation)
      const medR = group.medianColor.r;
      const medG = group.medianColor.g;
      const medB = group.medianColor.b;
      const brightness = (medR + medG + medB) / 3;
      const maxDiff = Math.max(
        Math.abs(medR - medG),
        Math.abs(medG - medB),
        Math.abs(medR - medB),
      );
      return !(brightness > 200 && maxDiff < 30); // Not white/gray background
    })
    .sort((a, b) => b.points.length - a.points.length)
    .slice(0, 6);

  if (boxes.length < 6) {
    throw new Error(
      "Could not detect all 6 color boxes. Please ensure the entire display is visible and well-lit.",
    );
  }

  // Step 4: Sort boxes by position (top-to-bottom, left-to-right)
  boxes.forEach((box) => {
    box.centerX =
      box.points.reduce((sum, p) => sum + p.x, 0) / box.points.length;
    box.centerY =
      box.points.reduce((sum, p) => sum + p.y, 0) / box.points.length;
  });

  boxes.sort((a, b) => {
    const rowA = Math.floor(a.centerY / (height / 2));
    const rowB = Math.floor(b.centerY / (height / 2));
    if (rowA !== rowB) return rowA - rowB;
    return a.centerX - b.centerX;
  });

  // Step 5: Draw detected regions on canvas for visual feedback
  ctx.strokeStyle = "lime";
  ctx.lineWidth = 3;
  boxes.forEach((box, idx) => {
    const minX = Math.min(...box.points.map((p) => p.x));
    const maxX = Math.max(...box.points.map((p) => p.x));
    const minY = Math.min(...box.points.map((p) => p.y));
    const maxY = Math.max(...box.points.map((p) => p.y));
    ctx.strokeRect(minX, minY, maxX - minX, maxY - minY);
  });

  // Step 6: Map to color names in expected order
  const colors = ["black", "white", "yellow", "red", "blue", "green"];
  const palette = {};

  boxes.forEach((box, idx) => {
    palette[colors[idx]] = {
      r: Math.round(box.medianColor.r),
      g: Math.round(box.medianColor.g),
      b: Math.round(box.medianColor.b),
    };
  });

  return palette;
}

function clusterColors(samples, k) {
  // Simple color clustering: group similar colors together
  const groups = [];
  const threshold = 40; // Color similarity threshold

  samples.forEach((sample) => {
    // Find closest existing group using temporary centroid
    let closestGroup = null;
    let minDist = Infinity;

    for (const group of groups) {
      const dr = sample.r - group.centroid.r;
      const dg = sample.g - group.centroid.g;
      const db = sample.b - group.centroid.b;
      const dist = Math.sqrt(dr * dr + dg * dg + db * db);

      if (dist < minDist && dist < threshold) {
        minDist = dist;
        closestGroup = group;
      }
    }

    if (closestGroup) {
      // Add to existing group and update centroid (running average)
      closestGroup.points.push(sample);
      const n = closestGroup.points.length;
      closestGroup.centroid.r =
        (closestGroup.centroid.r * (n - 1) + sample.r) / n;
      closestGroup.centroid.g =
        (closestGroup.centroid.g * (n - 1) + sample.g) / n;
      closestGroup.centroid.b =
        (closestGroup.centroid.b * (n - 1) + sample.b) / n;
    } else {
      // Create new group
      groups.push({
        centroid: { r: sample.r, g: sample.g, b: sample.b },
        points: [sample],
      });
    }
  });

  // Calculate median color for each group (more robust than mean for final result)
  groups.forEach((group) => {
    const rValues = group.points.map((p) => p.r).sort((a, b) => a - b);
    const gValues = group.points.map((p) => p.g).sort((a, b) => a - b);
    const bValues = group.points.map((p) => p.b).sort((a, b) => a - b);

    const mid = Math.floor(group.points.length / 2);
    if (group.points.length % 2 === 0) {
      // Even number of samples - average the two middle values
      group.medianColor = {
        r: (rValues[mid - 1] + rValues[mid]) / 2,
        g: (gValues[mid - 1] + gValues[mid]) / 2,
        b: (bValues[mid - 1] + bValues[mid]) / 2,
      };
    } else {
      // Odd number of samples - take the middle value
      group.medianColor = {
        r: rValues[mid],
        g: gValues[mid],
        b: bValues[mid],
      };
    }
  });

  return groups;
}

function displayMeasuredPalette(palette) {
  const container = document.getElementById("measuredPalette");
  container.innerHTML = "";

  const colors = ["black", "white", "yellow", "red", "blue", "green"];
  colors.forEach((color) => {
    const rgb = palette[color];
    const div = document.createElement("div");
    div.className = "palette-color";
    div.innerHTML = `
            <div class="palette-swatch" id="calibration-swatch-${color}" style="background-color: rgb(${rgb.r}, ${rgb.g}, ${rgb.b});"></div>
            <div class="palette-label">${color.charAt(0).toUpperCase() + color.slice(1)}</div>
            <div class="palette-rgb-inputs" style="display: flex; gap: 4px; margin-top: 5px;">
                <input type="number" id="calibration-r-${color}" min="0" max="255" value="${rgb.r}" 
                       style="width: 50px; padding: 2px; text-align: center;" 
                       data-color="${color}" data-channel="r" class="calibration-rgb-input">
                <input type="number" id="calibration-g-${color}" min="0" max="255" value="${rgb.g}" 
                       style="width: 50px; padding: 2px; text-align: center;" 
                       data-color="${color}" data-channel="g" class="calibration-rgb-input">
                <input type="number" id="calibration-b-${color}" min="0" max="255" value="${rgb.b}" 
                       style="width: 50px; padding: 2px; text-align: center;" 
                       data-color="${color}" data-channel="b" class="calibration-rgb-input">
            </div>
        `;
    container.appendChild(div);
  });

  // Add event listeners to update palette and swatch when values change
  document.querySelectorAll(".calibration-rgb-input").forEach((input) => {
    input.addEventListener("input", (e) => {
      const color = e.target.dataset.color;
      const channel = e.target.dataset.channel;
      let value = parseInt(e.target.value);

      // Clamp value between 0-255
      if (isNaN(value)) value = 0;
      if (value < 0) value = 0;
      if (value > 255) value = 255;
      e.target.value = value;

      // Update palette data
      measuredPaletteData[color][channel] = value;

      // Update swatch color (use calibration-specific ID)
      const swatch = document.getElementById(`calibration-swatch-${color}`);
      const r = measuredPaletteData[color].r;
      const g = measuredPaletteData[color].g;
      const b = measuredPaletteData[color].b;
      swatch.style.backgroundColor = `rgb(${r}, ${g}, ${b})`;
    });
  });
}

// Save calibration
document
  .getElementById("saveCalibrationBtn")
  .addEventListener("click", async () => {
    const statusDiv = document.getElementById("saveCalibrationStatus");
    statusDiv.textContent = "Saving calibration...";
    statusDiv.className = "status-info";

    try {
      const response = await fetch(`${API_BASE}/api/settings/palette`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(measuredPaletteData),
      });

      if (response.ok) {
        statusDiv.textContent = "Calibration saved successfully";
        statusDiv.className = "status-success";

        // Reload palette display and close calibration flow
        setTimeout(() => {
          loadColorPalette();
          document.getElementById("calibrationFlow").style.display = "none";
          document.getElementById("startCalibrationBtn").style.display =
            "inline-block";
        }, 1500);
      } else {
        throw new Error("Failed to save calibration");
      }
    } catch (error) {
      statusDiv.textContent = `Error: ${error.message}`;
      statusDiv.className = "status-error";
    }
  });

// Cancel calibration
document
  .getElementById("cancelCalibrationBtn")
  .addEventListener("click", () => {
    document.getElementById("calibrationFlow").style.display = "none";
    document.getElementById("startCalibrationBtn").style.display =
      "inline-block";
    measuredPaletteData = null;
  });

// Save current palette changes
document
  .getElementById("savePaletteBtn")
  .addEventListener("click", async () => {
    const statusDiv = document.getElementById("savePaletteStatus");
    statusDiv.textContent = "Saving palette...";
    statusDiv.className = "status-info";

    try {
      const response = await fetch(`${API_BASE}/api/settings/palette`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(currentPaletteData),
      });

      if (response.ok) {
        statusDiv.textContent = "Palette saved successfully";
        statusDiv.className = "status-success";

        // Update original data to match current
        originalPaletteData = JSON.parse(JSON.stringify(currentPaletteData));

        // Hide save button
        setTimeout(() => {
          document.getElementById("savePaletteButtonContainer").style.display =
            "none";
          statusDiv.innerHTML = "";
        }, 2000);
      } else {
        throw new Error("Failed to save palette");
      }
    } catch (error) {
      statusDiv.textContent = `Error: ${error.message}`;
      statusDiv.className = "status-error";
    }
  });

// Cancel current palette changes
document.getElementById("cancelPaletteBtn").addEventListener("click", () => {
  // Reload original palette
  currentPaletteData = JSON.parse(JSON.stringify(originalPaletteData));
  displayPalette(originalPaletteData);
  document.getElementById("savePaletteStatus").innerHTML = "";
});

// Reset palette to defaults
document
  .getElementById("resetPaletteBtn")
  .addEventListener("click", async () => {
    if (!confirm("Reset color palette to default values?")) return;

    try {
      const response = await fetch(`${API_BASE}/api/settings/palette`, {
        method: "DELETE",
      });

      if (response.ok) {
        // Reload palette from backend (which now has defaults)
        loadColorPalette();

        // Close calibration flow if it's open
        document.getElementById("calibrationFlow").style.display = "none";
        document.getElementById("startCalibrationBtn").style.display =
          "inline-block";
        measuredPaletteData = null;

        alert("Palette reset to defaults");
      } else {
        throw new Error("Failed to reset palette");
      }
    } catch (error) {
      alert("Error resetting palette: " + error.message);
    }
  });

// Load palette on page load
loadColorPalette();

// Rotate image button handler
document.getElementById("rotateBtn").addEventListener("click", async () => {
  const statusDiv = document.getElementById("rotateStatus");
  const btn = document.getElementById("rotateBtn");

  btn.disabled = true;
  statusDiv.textContent = "Rotating image...";
  statusDiv.className = "status-info";

  try {
    const response = await fetch(`${API_BASE}/api/rotate`, {
      method: "POST",
    });

    if (response.ok) {
      statusDiv.className = "status-success";
      statusDiv.textContent = "Image rotated successfully";

      // Reload images after a short delay to show the new image
      setTimeout(() => {
        loadImages();
      }, 1000);
    } else {
      throw new Error(`HTTP ${response.status}`);
    }
  } catch (error) {
    statusDiv.className = "status-error";
    statusDiv.textContent = "Failed to rotate image: " + error.message;
  } finally {
    btn.disabled = false;

    // Clear status message after 3 seconds
    setTimeout(() => {
      statusDiv.textContent = "";
      statusDiv.className = "";
    }, 3000);
  }
});

// OTA Update Functions
let otaStatusInterval = null;

async function loadOTAStatus() {
  try {
    const response = await fetch(`${API_BASE}/api/ota/status`);
    if (!response.ok) return;

    const data = await response.json();

    // Update version info
    document.getElementById("currentVersion").textContent =
      data.current_version;
    document.getElementById("latestVersion").textContent =
      data.latest_version || "-";

    // Update state info
    const stateInfo = document.getElementById("otaStateInfo");
    const checkBtn = document.getElementById("checkUpdateBtn");
    const installBtn = document.getElementById("installUpdateBtn");
    const progressContainer = document.getElementById("otaProgress");
    const progressBar = document.getElementById("otaProgressBar");

    // Reset classes
    stateInfo.className = "ota-state-info";

    switch (data.state) {
      case "idle":
        stateInfo.textContent = "";
        stateInfo.style.display = "none";
        checkBtn.disabled = false;
        installBtn.style.display = "none";
        progressContainer.style.display = "none";
        break;

      case "checking":
        stateInfo.textContent = "Checking for updates...";
        stateInfo.className = "ota-state-info checking";
        stateInfo.style.display = "block";
        checkBtn.disabled = true;
        installBtn.style.display = "none";
        progressContainer.style.display = "none";
        break;

      case "update_available":
        stateInfo.textContent = `Update available: ${data.latest_version}`;
        stateInfo.className = "ota-state-info update-available";
        stateInfo.style.display = "block";
        checkBtn.disabled = false;
        installBtn.style.display = "block";
        progressContainer.style.display = "none";
        break;

      case "downloading":
        stateInfo.textContent = "Downloading firmware...";
        stateInfo.className = "ota-state-info downloading";
        stateInfo.style.display = "block";
        checkBtn.disabled = true;
        installBtn.disabled = true;
        progressContainer.style.display = "block";
        progressBar.style.width = data.progress_percent + "%";
        progressBar.textContent = data.progress_percent + "%";
        break;

      case "installing":
        stateInfo.textContent = "Installing firmware...";
        stateInfo.className = "ota-state-info installing";
        stateInfo.style.display = "block";
        checkBtn.disabled = true;
        installBtn.disabled = true;
        progressContainer.style.display = "block";
        progressBar.style.width = data.progress_percent + "%";
        progressBar.textContent = data.progress_percent + "%";
        break;

      case "success":
        stateInfo.textContent = "Update successful! Device will reboot...";
        stateInfo.className = "ota-state-info success";
        stateInfo.style.display = "block";
        checkBtn.disabled = true;
        installBtn.disabled = true;
        progressContainer.style.display = "block";
        progressBar.style.width = "100%";
        progressBar.textContent = "100%";
        break;

      case "error":
        stateInfo.textContent =
          "Error: " + (data.error_message || "Update failed");
        stateInfo.className = "ota-state-info error";
        stateInfo.style.display = "block";
        checkBtn.disabled = false;
        installBtn.style.display = "none";
        progressContainer.style.display = "none";
        break;
    }
  } catch (error) {
    console.error("Failed to load OTA status:", error);
  }
}

async function checkForUpdate() {
  const statusDiv = document.getElementById("otaStatus");
  const checkBtn = document.getElementById("checkUpdateBtn");

  checkBtn.disabled = true;

  try {
    const response = await fetch(`${API_BASE}/api/ota/check`, {
      method: "POST",
    });

    if (!response.ok) {
      throw new Error(`HTTP ${response.status}`);
    }

    const data = await response.json();

    // Reload status to show results
    await loadOTAStatus();
  } catch (error) {
    statusDiv.textContent = "Failed to check for updates: " + error.message;
    statusDiv.className = "status-error";
  } finally {
    checkBtn.disabled = false;
    setTimeout(() => {
      statusDiv.textContent = "";
      statusDiv.className = "";
    }, 5000);
  }
}

async function installUpdate() {
  if (
    !confirm(
      "Install firmware update? The device will reboot after installation.",
    )
  ) {
    return;
  }

  const statusDiv = document.getElementById("otaStatus");
  const installBtn = document.getElementById("installUpdateBtn");

  installBtn.disabled = true;
  statusDiv.textContent = "Starting update...";
  statusDiv.className = "status-info";

  try {
    const response = await fetch(`${API_BASE}/api/ota/update`, {
      method: "POST",
    });

    if (!response.ok) {
      throw new Error(`HTTP ${response.status}`);
    }

    const data = await response.json();

    if (data.status === "success") {
      // Clear status message - the state info box will show progress
      statusDiv.textContent = "";
      statusDiv.className = "";

      // Start polling for status updates
      if (otaStatusInterval) {
        clearInterval(otaStatusInterval);
      }
      otaStatusInterval = setInterval(loadOTAStatus, 2000);
    } else {
      throw new Error(data.message || "Failed to start update");
    }
  } catch (error) {
    statusDiv.textContent = "Failed to start update: " + error.message;
    statusDiv.className = "status-error";
    installBtn.disabled = false;

    setTimeout(() => {
      statusDiv.textContent = "";
      statusDiv.className = "";
    }, 5000);
  }
}

// Load OTA status on page load and periodically
loadOTAStatus();
setInterval(loadOTAStatus, 10000); // Check every 10 seconds

// Make functions globally available
window.checkForUpdate = checkForUpdate;
window.installUpdate = installUpdate;
