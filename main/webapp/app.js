import { processImage, applyExposure, applyContrast, applySaturation, applyScurveTonemap } from './image-processor.js';

const API_BASE = '';

// Centralized default configuration for image processing
const DEFAULT_PARAMS = {
    exposure: 1.0,
    saturation: 1.3,
    toneMode: 'scurve',
    contrast: 1.0,
    strength: 0.9,
    shadowBoost: 0.0,
    highlightCompress: 1.5,
    midpoint: 0.5,
    colorMethod: 'rgb',
    renderMeasured: true,
    processingMode: 'enhanced'
};

let currentImages = [];
let selectedImage = null;
let currentAlbums = [];
let selectedAlbum = 'Default';

async function loadBatteryStatus() {
    try {
        const response = await fetch(`${API_BASE}/api/battery`);
        if (!response.ok || response.headers.get('content-type')?.includes('text/html')) {
            // Running in standalone mode without ESP32 backend
            return;
        }
        const data = await response.json();
        
        const batteryDiv = document.getElementById('batteryStatus');
        
        if (!data.battery_connected) {
            batteryDiv.innerHTML = '<span class="battery-disconnected">🔌 No Battery</span>';
            return;
        }
        
        const percent = data.battery_percent;
        const voltage = data.battery_voltage_mv;
        const charging = data.charging;
        
        let batteryIcon = '🔋';
        let batteryClass = 'battery-normal';
        
        if (charging) {
            batteryIcon = '⚡';
            batteryClass = 'battery-charging';
        } else if (percent < 20) {
            batteryClass = 'battery-low';
        } else if (percent < 50) {
            batteryClass = 'battery-medium';
        }
        
        batteryDiv.innerHTML = `
            <span class="${batteryClass}">
                ${batteryIcon} ${percent}% (${(voltage / 1000).toFixed(2)}V)
                ${charging ? ' Charging' : ''}
            </span>
        `;
    } catch (error) {
        // Silently fail if API not available (standalone mode)
        console.log('Battery API not available (standalone mode)');
    }
}

async function loadAlbums() {
    try {
        const response = await fetch(`${API_BASE}/api/albums`);
        if (!response.ok || response.headers.get('content-type')?.includes('text/html')) {
            console.log('Albums API not available (standalone mode)');
            return;
        }
        currentAlbums = await response.json();
        displayAlbums();
    } catch (error) {
        console.log('Failed to load albums:', error);
    }
}

function displayAlbums() {
    const albumList = document.getElementById('albumList');
    if (!albumList) return;
    
    albumList.innerHTML = '';
    
    // Sort albums: Default first, then alphabetically
    const sortedAlbums = [...currentAlbums].sort((a, b) => {
        if (a.name === 'Default') return -1;
        if (b.name === 'Default') return 1;
        return a.name.localeCompare(b.name);
    });
    
    // Update upload album selector
    const uploadAlbumSelect = document.getElementById('uploadAlbumSelect');
    if (uploadAlbumSelect) {
        uploadAlbumSelect.innerHTML = '';
        sortedAlbums.forEach(album => {
            const option = document.createElement('option');
            option.value = album.name;
            option.textContent = album.name;
            if (album.name === selectedAlbum) {
                option.selected = true;
            }
            uploadAlbumSelect.appendChild(option);
        });
    }
    
    sortedAlbums.forEach(album => {
        const item = document.createElement('div');
        item.className = 'album-item';
        if (album.name === selectedAlbum) {
            item.classList.add('selected');
        }
        
        const checkbox = document.createElement('input');
        checkbox.type = 'checkbox';
        checkbox.checked = album.enabled;
        checkbox.addEventListener('click', (e) => {
            e.stopPropagation();
        });
        checkbox.addEventListener('change', (e) => {
            toggleAlbumEnabled(album.name, e.target.checked);
        });
        
        const name = document.createElement('span');
        name.textContent = album.name;
        
        const deleteBtn = document.createElement('button');
        deleteBtn.className = 'delete-btn';
        deleteBtn.textContent = '×';
        deleteBtn.title = 'Delete album';
        if (album.name === 'Default') {
            deleteBtn.disabled = true;
            deleteBtn.style.visibility = 'hidden';
        }
        deleteBtn.addEventListener('click', (e) => {
            e.stopPropagation();
            deleteAlbum(album.name);
        });
        
        item.appendChild(checkbox);
        item.appendChild(name);
        item.appendChild(deleteBtn);
        item.addEventListener('click', () => selectAlbum(album.name));
        
        albumList.appendChild(item);
    });
}

function selectAlbum(albumName) {
    selectedAlbum = albumName;
    displayAlbums();
    loadImagesForAlbum(albumName);
    
    // Update album name display
    const albumNameElement = document.getElementById('currentAlbumName');
    if (albumNameElement) {
        albumNameElement.textContent = `${albumName}`;
    }
    
    // Update upload album selector
    const uploadAlbumSelect = document.getElementById('uploadAlbumSelect');
    if (uploadAlbumSelect) {
        uploadAlbumSelect.value = albumName;
    }
}

async function loadImagesForAlbum(albumName) {
    try {
        const response = await fetch(`${API_BASE}/api/images?album=${encodeURIComponent(albumName)}`);
        if (!response.ok) {
            console.log('Failed to load images for album');
            return;
        }
        currentImages = await response.json();
        displayImages();
    } catch (error) {
        console.log('Failed to load images:', error);
    }
}

async function toggleAlbumEnabled(albumName, enabled) {
    try {
        const response = await fetch(`${API_BASE}/api/albums/enabled?name=${encodeURIComponent(albumName)}`, {
            method: 'PUT',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ enabled })
        });
        if (response.ok) {
            console.log(`Album ${albumName} ${enabled ? 'enabled' : 'disabled'}`);
            loadAlbums();
        }
    } catch (error) {
        console.error('Failed to toggle album:', error);
    }
}

async function createAlbum() {
    const name = prompt('Enter album name:');
    if (!name || name.trim() === '') return;
    
    try {
        const response = await fetch(`${API_BASE}/api/albums`, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ name: name.trim() })
        });
        if (response.ok) {
            console.log('Album created:', name);
            loadAlbums();
        } else {
            alert('Failed to create album');
        }
    } catch (error) {
        console.error('Failed to create album:', error);
    }
}

async function deleteAlbum(albumName) {
    if (albumName === 'Default') {
        alert('Cannot delete Default album');
        return;
    }
    if (!confirm(`Delete album "${albumName}" and all its images?`)) return;
    
    try {
        const response = await fetch(`${API_BASE}/api/albums?name=${encodeURIComponent(albumName)}`, {
            method: 'DELETE'
        });
        if (response.ok) {
            console.log('Album deleted:', albumName);
            if (selectedAlbum === albumName) {
                selectedAlbum = 'Default';
            }
            loadAlbums();
            loadImagesForAlbum(selectedAlbum);
        } else {
            alert('Failed to delete album');
        }
    } catch (error) {
        console.error('Failed to delete album:', error);
    }
}

async function loadImages() {
    loadImagesForAlbum(selectedAlbum);
}

function displayImages() {
    const imageList = document.getElementById('imageList');
    
    imageList.innerHTML = '';
    
    // Add upload button as first item
    const uploadItem = document.createElement('div');
    uploadItem.className = 'image-item upload-item';
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
    uploadItem.addEventListener('click', () => {
        document.getElementById('fileInput').click();
    });
    imageList.appendChild(uploadItem);
    
    if (currentImages.length === 0) {
        return;
    }
    
    currentImages.forEach(image => {
        const item = document.createElement('div');
        item.className = 'image-item';
        
        const thumbnail = document.createElement('img');
        thumbnail.className = 'image-thumbnail';
        // Convert .bmp to .jpg for thumbnail
        const thumbnailName = image.name.replace(/\.bmp$/i, '.jpg');
        // Use album/filename format
        const thumbnailPath = `${selectedAlbum}/${thumbnailName}`;
        thumbnail.src = `${API_BASE}/api/image?name=${encodeURIComponent(thumbnailPath)}`;
        thumbnail.alt = image.name;
        thumbnail.loading = 'lazy';
        
        const info = document.createElement('div');
        info.className = 'image-info';
        
        const name = document.createElement('div');
        name.className = 'image-name';
        name.textContent = image.name;
        
        info.appendChild(name);
        
        const deleteBtn = document.createElement('button');
        deleteBtn.className = 'delete-btn';
        deleteBtn.textContent = '×';
        deleteBtn.title = 'Delete image';
        deleteBtn.addEventListener('click', (e) => {
            e.stopPropagation();
            deleteImage(image.name);
        });
        
        item.appendChild(thumbnail);
        item.appendChild(info);
        item.appendChild(deleteBtn);
        
        item.addEventListener('click', () => selectImage(image.name, item));
        
        imageList.appendChild(item);
    });
}

function formatFileSize(bytes) {
    if (bytes < 1024) return bytes + ' B';
    if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + ' KB';
    return (bytes / (1024 * 1024)).toFixed(1) + ' MB';
}

async function deleteImage(filename) {
    if (!confirm(`Are you sure you want to delete "${filename}"?`)) {
        return;
    }
    
    try {
        // Use album/filename format
        const fullPath = `${selectedAlbum}/${filename}`;
        const response = await fetch(`${API_BASE}/api/delete`, {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify({ filename: fullPath })
        });
        
        const data = await response.json();
        
        if (data.status === 'success') {
            console.log('Image deleted:', filename);
            loadImages(); // Reload the image list
        } else {
            alert('Failed to delete image');
        }
    } catch (error) {
        console.error('Error deleting image:', error);
        alert('Error deleting image');
    }
}

let isDisplaying = false;

async function selectImage(filename, element) {
    if (isDisplaying) {
        alert('Please wait for the current display operation to complete');
        return;
    }
    
    // Show confirmation dialog
    const confirmed = confirm(`Display "${filename}" on the e-paper screen?\n\nThis will take approximately 40 seconds to update.`);
    if (!confirmed) {
        return;
    }
    
    // Update selection
    document.querySelectorAll('.image-item').forEach(item => {
        item.classList.remove('selected');
    });
    element.classList.add('selected');
    selectedImage = filename;
    
    isDisplaying = true;
    
    // Show loading indicator
    const displayStatus = document.getElementById('displayStatus');
    displayStatus.style.display = 'block';
    
    try {
        // Use album/filename format
        const fullPath = `${selectedAlbum}/${filename}`;
        const response = await fetch(`${API_BASE}/api/display`, {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify({ filename: fullPath })
        });
        
        const data = await response.json();
        
        if (data.status === 'success') {
            console.log('Image displayed:', filename);
        } else if (data.status === 'busy') {
            alert('Display is currently updating, please wait');
        } else {
            alert('Failed to display image');
        }
    } catch (error) {
        console.error('Error displaying image:', error);
        alert('Error displaying image');
    } finally {
        // Hide loading indicator and re-enable display
        displayStatus.style.display = 'none';
        isDisplaying = false;
    }
}

// Global state for image processing
let currentImageFile = null;
let currentImageCanvas = null;
let originalImageData = null; // Store original unprocessed image data
let currentParams = { ...DEFAULT_PARAMS };

document.getElementById('fileInput').addEventListener('change', async (e) => {
    const file = e.target.files[0];
    if (!file) return;
    
    // Store the file and show preview
    currentImageFile = file;
    
    // Show image processing section and preview area
    document.getElementById('imageProcessingSection').style.display = 'block';
    document.getElementById('previewArea').style.display = 'block';
    
    // Ensure controls and buttons are visible (in case they were hidden from previous upload)
    document.querySelector('.button-group').style.display = 'flex';
    document.querySelector('.controls-grid').style.display = 'grid';
    document.getElementById('uploadProgress').style.display = 'none';
    
    await loadImagePreview(file);
});

async function resizeImage(file, maxWidth, maxHeight, quality) {
    return new Promise((resolve, reject) => {
        const reader = new FileReader();
        reader.onload = (e) => {
            const img = new Image();
            img.onload = () => {
                const canvas = document.createElement('canvas');
                const origWidth = img.width;
                const origHeight = img.height;
                
                // Determine if portrait or landscape
                const isPortrait = origHeight > origWidth;
                
                // Set canvas to exact display dimensions
                if (isPortrait) {
                    canvas.width = maxHeight;  // 480
                    canvas.height = maxWidth;  // 800
                } else {
                    canvas.width = maxWidth;   // 800
                    canvas.height = maxHeight; // 480
                }
                
                // Calculate scale to COVER (fill) the canvas
                const scaleX = canvas.width / origWidth;
                const scaleY = canvas.height / origHeight;
                const scale = Math.max(scaleX, scaleY);
                
                const scaledWidth = Math.round(origWidth * scale);
                const scaledHeight = Math.round(origHeight * scale);
                
                // Center and crop
                const offsetX = (canvas.width - scaledWidth) / 2;
                const offsetY = (canvas.height - scaledHeight) / 2;
                
                const ctx = canvas.getContext('2d');
                ctx.drawImage(img, offsetX, offsetY, scaledWidth, scaledHeight);
                
                canvas.toBlob((blob) => {
                    resolve(blob);
                }, 'image/jpeg', quality);
            };
            img.onerror = reject;
            img.src = e.target.result;
        };
        reader.onerror = reject;
        reader.readAsDataURL(file);
    });
}

async function loadImagePreview(file) {
    const previewCanvas = document.getElementById('previewCanvas');
    const originalCanvas = document.getElementById('originalCanvas');
    const statusDiv = document.getElementById('uploadStatus');
    
    // Clear any previous status
    statusDiv.textContent = '';
    statusDiv.className = '';
    
    try {
        // Load image and resize to display size
        const img = await loadImage(file);
        
        // Determine if portrait or landscape
        const isPortrait = img.height > img.width;
        
        // Set canvas to display dimensions (800x480 or 480x800)
        const width = isPortrait ? 480 : 800;
        const height = isPortrait ? 800 : 480;
        
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
        const previewCtx = previewCanvas.getContext('2d', { willReadFrequently: true });
        const originalCtx = originalCanvas.getContext('2d');
        
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
        console.error('Error loading preview:', error);
        statusDiv.className = 'status-error';
        statusDiv.textContent = 'Error loading image preview';
    }
}

function loadImage(file) {
    return new Promise((resolve, reject) => {
        const reader = new FileReader();
        reader.onload = (e) => {
            const img = new Image();
            img.onload = () => resolve(img);
            img.onerror = reject;
            img.src = e.target.result;
        };
        reader.onerror = reject;
        reader.readAsDataURL(file);
    });
}

function initComparisonSlider() {
    const sliderContainer = document.getElementById('sliderContainer');
    const comparisonWrapper = document.getElementById('comparisonWrapper');
    const previewCanvas = document.getElementById('previewCanvas');
    
    if (!sliderContainer || !comparisonWrapper || !previewCanvas) return;
    
    let isDragging = false;
    
    function updateSliderPosition(clientX) {
        const rect = comparisonWrapper.getBoundingClientRect();
        let position = ((clientX - rect.left) / rect.width) * 100;
        position = Math.max(0, Math.min(100, position));
        
        sliderContainer.style.left = position + '%';
        // Clip from left: at 80% position, clip 80% from left, showing 20% dithered on right
        // inset(top right bottom left)
        previewCanvas.style.clipPath = `inset(0 0 0 ${position}%)`;
    }
    
    // Mouse events
    sliderContainer.addEventListener('mousedown', (e) => {
        isDragging = true;
        e.preventDefault();
    });
    
    document.addEventListener('mousemove', (e) => {
        if (isDragging) {
            updateSliderPosition(e.clientX);
        }
    });
    
    document.addEventListener('mouseup', () => {
        isDragging = false;
    });
    
    // Touch events
    sliderContainer.addEventListener('touchstart', (e) => {
        isDragging = true;
        e.preventDefault();
    });
    
    document.addEventListener('touchmove', (e) => {
        if (isDragging && e.touches.length > 0) {
            updateSliderPosition(e.touches[0].clientX);
        }
    });
    
    document.addEventListener('touchend', () => {
        isDragging = false;
    });
    
    // Click on wrapper to move slider
    comparisonWrapper.addEventListener('click', (e) => {
        if (e.target !== sliderContainer && !sliderContainer.contains(e.target)) {
            updateSliderPosition(e.clientX);
        }
    });
    
    // Initialize at 0% (slider on left, showing full dithered preview)
    sliderContainer.style.left = '0%';
    previewCanvas.style.clipPath = 'inset(0 0 0 0%)';
}

function drawCurveVisualization() {
    const canvas = document.getElementById('curveCanvas');
    const ctx = canvas.getContext('2d');
    const width = canvas.width;
    const height = canvas.height;
    const padding = 40;
    const graphWidth = width - 2 * padding;
    const graphHeight = height - 2 * padding;
    
    // Clear canvas
    ctx.fillStyle = 'white';
    ctx.fillRect(0, 0, width, height);
    
    // Draw axes
    ctx.strokeStyle = '#333';
    ctx.lineWidth = 2;
    ctx.beginPath();
    ctx.moveTo(padding, padding);
    ctx.lineTo(padding, height - padding);
    ctx.lineTo(width - padding, height - padding);
    ctx.stroke();
    
    // Draw axis labels
    ctx.fillStyle = '#333';
    ctx.font = '12px sans-serif';
    ctx.textAlign = 'center';
    ctx.fillText('Input', width / 2, height - 10);
    ctx.save();
    ctx.translate(15, height / 2);
    ctx.rotate(-Math.PI / 2);
    ctx.fillText('Output', 0, 0);
    ctx.restore();
    
    // Draw grid lines
    ctx.strokeStyle = '#e0e0e0';
    ctx.lineWidth = 1;
    for (let i = 0; i <= 4; i++) {
        const x = padding + (graphWidth * i / 4);
        const y = padding + (graphHeight * i / 4);
        
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
    ctx.strokeStyle = '#ccc';
    ctx.lineWidth = 1;
    ctx.setLineDash([5, 5]);
    ctx.beginPath();
    ctx.moveTo(padding, height - padding);
    ctx.lineTo(width - padding, padding);
    ctx.stroke();
    ctx.setLineDash([]);
    
    // Draw S-curve
    ctx.strokeStyle = '#667eea';
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
            result = midpoint + Math.pow(highlightVal, 1.0 + strength * highlightCompress) * (1.0 - midpoint);
        }
        
        const x = padding + i;
        const y = height - padding - (result * graphHeight);
        
        if (i === 0) {
            ctx.moveTo(x, y);
        } else {
            ctx.lineTo(x, y);
        }
    }
    
    ctx.stroke();
    
    // Draw midpoint indicator
    ctx.strokeStyle = '#ff6b6b';
    ctx.lineWidth = 2;
    ctx.setLineDash([3, 3]);
    const midX = padding + midpoint * graphWidth;
    ctx.beginPath();
    ctx.moveTo(midX, padding);
    ctx.lineTo(midX, height - padding);
    ctx.stroke();
    ctx.setLineDash([]);
    
    // Midpoint label
    ctx.fillStyle = '#ff6b6b';
    ctx.font = '11px sans-serif';
    ctx.textAlign = 'center';
    ctx.fillText('midpoint', midX, padding - 5);
}

function updatePreview() {
if (!currentImageCanvas || !originalImageData) return;
    
// Update curve visualization
drawCurveVisualization();
    
// Create a copy of the original image data
const tempCanvas = document.createElement('canvas');
tempCanvas.width = originalImageData.width;
tempCanvas.height = originalImageData.height;
const tempCtx = tempCanvas.getContext('2d', { willReadFrequently: true });
    
// Copy original image data
const imageDataCopy = tempCtx.createImageData(originalImageData.width, originalImageData.height);
imageDataCopy.data.set(originalImageData.data);
    
// Apply image processing to the ImageData copy
processImage(imageDataCopy, currentParams);
    
// Put processed data back to temp canvas
tempCtx.putImageData(imageDataCopy, 0, 0);
    
// Draw processed image to preview canvas
const previewCanvas = document.getElementById('previewCanvas');
const ctx = previewCanvas.getContext('2d');
ctx.drawImage(tempCanvas, 0, 0);
}

// Parameter change handlers
document.getElementById('exposure').addEventListener('input', (e) => {
    currentParams.exposure = parseFloat(e.target.value);
    document.getElementById('exposureValue').textContent = e.target.value;
    updatePreview();
});

document.getElementById('saturation').addEventListener('input', (e) => {
    currentParams.saturation = parseFloat(e.target.value);
    document.getElementById('saturationValue').textContent = e.target.value;
    updatePreview();
});

document.getElementById('contrast').addEventListener('input', (e) => {
    currentParams.contrast = parseFloat(e.target.value);
    document.getElementById('contrastValue').textContent = e.target.value;
    updatePreview();
});

document.getElementById('scurveStrength').addEventListener('input', (e) => {
    currentParams.strength = parseFloat(e.target.value);
    document.getElementById('strengthValue').textContent = e.target.value;
    updatePreview();
});

document.getElementById('scurveShadow').addEventListener('input', (e) => {
    currentParams.shadowBoost = parseFloat(e.target.value);
    document.getElementById('shadowValue').textContent = e.target.value;
    updatePreview();
});

document.getElementById('scurveHighlight').addEventListener('input', (e) => {
    currentParams.highlightCompress = parseFloat(e.target.value);
    document.getElementById('highlightValue').textContent = e.target.value;
    updatePreview();
});

document.getElementById('scurveMidpoint').addEventListener('input', (e) => {
    currentParams.midpoint = parseFloat(e.target.value);
    document.getElementById('midpointValue').textContent = e.target.value;
    updatePreview();
});

// Color matching method radio buttons
document.querySelectorAll('input[name="colorMethod"]').forEach(radio => {
    radio.addEventListener('change', (e) => {
        currentParams.colorMethod = e.target.value;
        updatePreview();
    });
});

// Tone mode radio buttons
document.querySelectorAll('input[name="toneMode"]').forEach(radio => {
    radio.addEventListener('change', (e) => {
        currentParams.toneMode = e.target.value;
        
        // Show/hide contrast or S-curve controls based on mode
        const contrastControl = document.getElementById('contrastControl');
        const curveCanvasWrapper = document.querySelector('.curve-canvas-wrapper');
        
        if (e.target.value === 'contrast') {
            contrastControl.style.display = 'block';
            curveCanvasWrapper.style.display = 'none';
        } else {
            contrastControl.style.display = 'none';
            curveCanvasWrapper.style.display = 'flex';
        }
        
        updatePreview();
    });
});

// Processing mode radio buttons
document.querySelectorAll('input[name="processingMode"]').forEach(radio => {
    radio.addEventListener('change', (e) => {
        currentParams.processingMode = e.target.value;
        
        // Show/hide enhanced controls and canvas based on mode
        const enhancedControls = document.getElementById('enhancedControls');
        const colorMethodControl = document.getElementById('colorMethodControl');
        const curveCanvasWrapper = document.querySelector('.curve-canvas-wrapper');
        if (e.target.value === 'stock') {
            enhancedControls.style.display = 'none';
            colorMethodControl.style.display = 'none';
            curveCanvasWrapper.style.display = 'none';
        } else {
            enhancedControls.style.display = 'grid';
            colorMethodControl.style.display = 'block';
            // Show curve canvas only if S-curve mode is selected
            if (currentParams.toneMode === 'scurve') {
                curveCanvasWrapper.style.display = 'block';
            }
        }
        
        updatePreview();
    });
});

// Discard image and return to upload
document.getElementById('discardImage').addEventListener('click', () => {
    currentImageFile = null;
    currentImageCanvas = null;
    originalImageData = null;
    document.getElementById('fileInput').value = '';
    document.getElementById('fileName').textContent = '';
    // Section is already visible, just showing upload area
    document.getElementById('uploadArea').style.display = 'block';
    document.getElementById('previewArea').style.display = 'none';
    document.getElementById('uploadStatus').textContent = '';
    document.getElementById('uploadStatus').className = '';
});

// Reset to defaults
document.getElementById('resetParams').addEventListener('click', () => {
    currentParams = { ...DEFAULT_PARAMS };
    
    document.getElementById('exposure').value = DEFAULT_PARAMS.exposure;
    document.getElementById('exposureValue').textContent = DEFAULT_PARAMS.exposure;
    document.getElementById('saturation').value = DEFAULT_PARAMS.saturation;
    document.getElementById('saturationValue').textContent = DEFAULT_PARAMS.saturation;
    document.getElementById('contrast').value = DEFAULT_PARAMS.contrast;
    document.getElementById('contrastValue').textContent = DEFAULT_PARAMS.contrast;
    document.getElementById('scurveStrength').value = DEFAULT_PARAMS.strength;
    document.getElementById('strengthValue').textContent = DEFAULT_PARAMS.strength;
    document.getElementById('scurveShadow').value = DEFAULT_PARAMS.shadowBoost;
    document.getElementById('shadowValue').textContent = DEFAULT_PARAMS.shadowBoost;
    document.getElementById('scurveHighlight').value = DEFAULT_PARAMS.highlightCompress;
    document.getElementById('highlightValue').textContent = DEFAULT_PARAMS.highlightCompress;
    document.getElementById('scurveMidpoint').value = DEFAULT_PARAMS.midpoint;
    document.getElementById('midpointValue').textContent = DEFAULT_PARAMS.midpoint;
    document.querySelector(`input[name="colorMethod"][value="${DEFAULT_PARAMS.colorMethod}"]`).checked = true;
    document.querySelector(`input[name="toneMode"][value="${DEFAULT_PARAMS.toneMode}"]`).checked = true;
    document.querySelector(`input[name="processingMode"][value="${DEFAULT_PARAMS.processingMode}"]`).checked = true;
    document.getElementById('enhancedControls').style.display = 'grid';
    document.getElementById('colorMethodControl').style.display = 'block';
    document.getElementById('contrastControl').style.display = 'none';
    document.querySelector('.curve-canvas-wrapper').style.display = 'flex';
    
    updatePreview();
});

// Upload processed image
document.getElementById('uploadProcessed').addEventListener('click', async () => {
    if (!currentImageFile || !currentImageCanvas) {
        alert('No image loaded');
        return;
    }
    
    const statusDiv = document.getElementById('uploadStatus');
    const uploadProgress = document.getElementById('uploadProgress');
    const buttonGroup = document.querySelector('.button-group');
    const controlsGrid = document.querySelector('.controls-grid');
    
    // Hide buttons and controls, show progress
    buttonGroup.style.display = 'none';
    controlsGrid.style.display = 'none';
    uploadProgress.style.display = 'block';
    statusDiv.textContent = '';
    statusDiv.className = '';
    
    // Wait for UI to update
    await new Promise(resolve => setTimeout(resolve, 0));
    
    let uploadSucceeded = false;
    
    try {
        let imageBlob;
        
        if (currentParams.processingMode === 'stock') {
            // Stock mode: send raw scaled/cropped image (no processing)
            imageBlob = await resizeImage(currentImageFile, 800, 480, 0.90);
        } else {
            // Enhanced mode: apply exposure, saturation, and tone mapping, but NO dithering
            // Create a temporary canvas for processing
            const tempCanvas = document.createElement('canvas');
            tempCanvas.width = originalImageData.width;
            tempCanvas.height = originalImageData.height;
            const tempCtx = tempCanvas.getContext('2d', { willReadFrequently: true });
            
            // Copy original image data
            const imageDataCopy = tempCtx.createImageData(originalImageData.width, originalImageData.height);
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
            if (currentParams.toneMode === 'contrast') {
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
                    currentParams.midpoint
                );
            }
            
            // Put processed data to canvas (NO dithering)
            tempCtx.putImageData(imageDataCopy, 0, 0);
            
            // Convert to blob
            imageBlob = await new Promise(resolve => {
                tempCanvas.toBlob(resolve, 'image/jpeg', 0.90);
            });
        }
        
        // Create thumbnail (200x120 or 120x200) from original
        const thumbnailBlob = await resizeImage(currentImageFile, 200, 120, 0.85);
        
        const formData = new FormData();
        // Send text fields first so backend can parse them before processing files
        formData.append('album', selectedAlbum);
        formData.append('processingMode', currentParams.processingMode);
        formData.append('image', imageBlob, currentImageFile.name);
        formData.append('thumbnail', thumbnailBlob, 'thumb_' + currentImageFile.name);
        
        const response = await fetch(`${API_BASE}/api/upload`, {
            method: 'POST',
            body: formData
        });
        
        const data = await response.json();
        
        if (data.status === 'success') {
            uploadSucceeded = true;
            
            // Reset state
            currentImageFile = null;
            currentImageCanvas = null;
            originalImageData = null;
            document.getElementById('fileInput').value = '';
            
            // Hide processing section
            document.getElementById('imageProcessingSection').style.display = 'none';
            document.getElementById('previewArea').style.display = 'none';
            uploadProgress.style.display = 'none';
            
            // Refresh image gallery
            loadImages();
        } else {
            statusDiv.className = 'status-error';
            statusDiv.textContent = 'Upload failed';
        }
    } catch (error) {
        console.error('Error uploading:', error);
        statusDiv.className = 'status-error';
        statusDiv.textContent = 'Error uploading file';
    } finally {
        // Only show controls if upload failed (still on preview)
        if (!uploadSucceeded) {
            buttonGroup.style.display = 'flex';
            controlsGrid.style.display = 'grid';
            uploadProgress.style.display = 'none';
        }
    }
});

async function loadConfig() {
    try {
        const response = await fetch(`${API_BASE}/api/config`);
        if (!response.ok || response.headers.get('content-type')?.includes('text/html')) {
            // Running in standalone mode without ESP32 backend
            console.log('Config API not available (standalone mode)');
            return;
        }
        const data = await response.json();
        
        document.getElementById('autoRotate').checked = data.auto_rotate || false;
        document.getElementById('rotateInterval').value = data.rotate_interval || 3600;
        document.getElementById('deepSleepEnabled').checked = data.deep_sleep_enabled !== false;
        
        // Update warning visibility based on loaded state
        updateDeepSleepWarning();
    } catch (error) {
        // Silently fail if API not available (standalone mode)
        console.log('Config API not available (standalone mode)');
    }
}

// Toggle deep sleep warning visibility based on checkbox state
function updateDeepSleepWarning() {
    const deepSleepEnabled = document.getElementById('deepSleepEnabled').checked;
    const warning = document.getElementById('deepSleepWarning');
    warning.style.display = deepSleepEnabled ? 'none' : 'block';
}

// Add event listener to deep sleep checkbox
document.getElementById('deepSleepEnabled').addEventListener('change', updateDeepSleepWarning);

document.getElementById('configForm').addEventListener('submit', async (e) => {
    e.preventDefault();
    
    const statusDiv = document.getElementById('configStatus');
    const autoRotate = document.getElementById('autoRotate').checked;
    const rotateInterval = parseInt(document.getElementById('rotateInterval').value);
    const deepSleepEnabled = document.getElementById('deepSleepEnabled').checked;
    
    try {
        const response = await fetch(`${API_BASE}/api/config`, {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify({
                auto_rotate: autoRotate,
                rotate_interval: rotateInterval,
                deep_sleep_enabled: deepSleepEnabled
            })
        });
        
        const data = await response.json();
        
        if (data.status === 'success') {
            statusDiv.className = 'status-success';
            statusDiv.textContent = 'Settings saved successfully';
        } else {
            statusDiv.className = 'status-error';
            statusDiv.textContent = 'Failed to save settings';
        }
    } catch (error) {
        console.error('Error saving config:', error);
        statusDiv.className = 'status-error';
        statusDiv.textContent = 'Error saving settings';
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
        if (!response.ok || response.headers.get('content-type')?.includes('text/html')) {
            // Running in standalone mode without ESP32 backend
            console.log('Version API not available (standalone mode)');
            return;
        }
        const data = await response.json();
        
        // Update footer with version
        const footer = document.querySelector('footer p');
        if (footer && data.version) {
            footer.textContent = `ESP32-S3 PhotoFrame v${data.version}`;
        }
    } catch (error) {
        // Silently fail if API not available (standalone mode)
        console.log('Version API not available (standalone mode)');
    }
}

// Listen for page visibility changes
document.addEventListener('visibilitychange', () => {
    if (document.hidden) {
        console.log('Page hidden - stopping periodic updates');
        stopPeriodicUpdates();
    } else {
        console.log('Page visible - starting periodic updates');
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
    const imageList = document.getElementById('imageList');
    if (!imageList) return;
    
    ['dragenter', 'dragover', 'dragleave', 'drop'].forEach(eventName => {
        imageList.addEventListener(eventName, preventDefaults, false);
    });
    
    function preventDefaults(e) {
        e.preventDefault();
        e.stopPropagation();
    }
    
    ['dragenter', 'dragover'].forEach(eventName => {
        imageList.addEventListener(eventName, () => {
            imageList.classList.add('drag-over');
        }, false);
    });
    
    ['dragleave', 'drop'].forEach(eventName => {
        imageList.addEventListener(eventName, () => {
            imageList.classList.remove('drag-over');
        }, false);
    });
    
    imageList.addEventListener('drop', handleDrop, false);
    
    function handleDrop(e) {
        const dt = e.dataTransfer;
        const files = dt.files;
        
        if (files.length > 0) {
            const file = files[0];
            if (file.type === 'image/jpeg' || file.type === 'image/jpg') {
                const fileInput = document.getElementById('fileInput');
                const dataTransfer = new DataTransfer();
                dataTransfer.items.add(file);
                fileInput.files = dataTransfer.files;
                
                // Trigger file input change event
                const event = new Event('change', { bubbles: true });
                fileInput.dispatchEvent(event);
            } else {
                alert('Please upload a JPG/JPEG image');
            }
        }
    }
}

// Initialize form controls with default values on page load
function initializeFormDefaults() {
    document.getElementById('exposure').value = DEFAULT_PARAMS.exposure;
    document.getElementById('exposureValue').textContent = DEFAULT_PARAMS.exposure;
    document.getElementById('saturation').value = DEFAULT_PARAMS.saturation;
    document.getElementById('saturationValue').textContent = DEFAULT_PARAMS.saturation;
    document.getElementById('contrast').value = DEFAULT_PARAMS.contrast;
    document.getElementById('contrastValue').textContent = DEFAULT_PARAMS.contrast;
    document.getElementById('scurveStrength').value = DEFAULT_PARAMS.strength;
    document.getElementById('strengthValue').textContent = DEFAULT_PARAMS.strength;
    document.getElementById('scurveShadow').value = DEFAULT_PARAMS.shadowBoost;
    document.getElementById('shadowValue').textContent = DEFAULT_PARAMS.shadowBoost;
    document.getElementById('scurveHighlight').value = DEFAULT_PARAMS.highlightCompress;
    document.getElementById('highlightValue').textContent = DEFAULT_PARAMS.highlightCompress;
    document.getElementById('scurveMidpoint').value = DEFAULT_PARAMS.midpoint;
    document.getElementById('midpointValue').textContent = DEFAULT_PARAMS.midpoint;
    document.querySelector(`input[name="colorMethod"][value="${DEFAULT_PARAMS.colorMethod}"]`).checked = true;
    document.querySelector(`input[name="toneMode"][value="${DEFAULT_PARAMS.toneMode}"]`).checked = true;
    document.querySelector(`input[name="processingMode"][value="${DEFAULT_PARAMS.processingMode}"]`).checked = true;
}

setupDragAndDrop();
initializeFormDefaults();
